/*
 * mfrc522.chip.c - a Wokwi custom chip that behaves like an MFRC522 RFID reader
 * (only the parts our firmware uses). Wokwi has no RFID part, so we made one.
 *
 * How the real MFRC522 talks over SPI:
 *   - The ESP32 pulls SDA (chip select) LOW, sends an address byte, then data.
 *   - Address byte = (reg << 1) & 0x7E, plus 0x80 when it is a READ.
 *   - READ:  the register value comes back during the NEXT byte.
 *   - WRITE: every byte after the address is written to that register
 *            (several bytes in a row to FIFODataReg fill the FIFO).
 *
 * How a card is "read":
 *   The ESP32 puts a card command in the FIFO, sets CommandReg = Transceive and
 *   then sets the StartSend bit in BitFramingReg. We then answer as if a card
 *   (chosen with the "card" slider) had replied: the answer goes into the FIFO
 *   and ComIrqReg says "received" (RxIRq) or "timeout, no card" (TimerIRq).
 *
 * Card slider: 0 = no card, 1..7 = one of the cards in uids[] below.
 *
 * Two readers share one SPI bus, so MISO is only driven while SDA is LOW.
 * Build: ./build.sh  (needs wasi-sdk)
 */
#include "wokwi-api.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- MFRC522 register numbers (datasheet section 9) ---- */
#define CommandReg     0x01
#define ComIrqReg      0x04
#define DivIrqReg      0x05
#define ErrorReg       0x06
#define FIFODataReg    0x09
#define FIFOLevelReg   0x0A
#define ControlReg     0x0C
#define BitFramingReg  0x0D
#define CRCResultRegH  0x21
#define CRCResultRegL  0x22
#define VersionReg     0x37

/* ---- CommandReg commands ---- */
#define CMD_IDLE        0x00
#define CMD_CALCCRC     0x03
#define CMD_TRANSCEIVE  0x0C
#define CMD_SOFTRESET   0x0F

/* ---- ComIrqReg bits ---- */
#define IRQ_TIMER  0x01
#define IRQ_IDLE   0x10
#define IRQ_RX     0x20

/* ---- card commands (ISO 14443A) ---- */
#define PICC_REQA     0x26
#define PICC_WUPA     0x52
#define PICC_SEL_CL1  0x93
#define PICC_HALT     0x50

#define FIFO_SIZE 64

/* The cards on the slider (1..7). They match access_control.c. */
static const uint8_t uids[7][4] = {
    {0xA1, 0xB2, 0xC3, 0xD4},   /* 1 Alice  (Staff)   */
    {0xB2, 0xC3, 0xD4, 0xE5},   /* 2 Brian  (Student) */
    {0xC3, 0xD4, 0xE5, 0xF6},   /* 3 Carol  (Student) */
    {0xD4, 0xE5, 0xF6, 0xA7},   /* 4 Dan    (Admin)   */
    {0xE5, 0xF6, 0xA7, 0xB8},   /* 5 Grace  (Guest)   */
    {0xDE, 0xAD, 0xBE, 0xEF},   /* 6 Lost card (blocked) */
    {0x12, 0x34, 0x56, 0x78},   /* 7 unknown card     */
};

typedef struct {
    pin_t cs;            /* SDA pin = chip select, active LOW */
    pin_t miso;
    spi_dev_t spi;
    uint32_t card_attr;  /* the "card" slider */

    uint8_t regs[64];    /* plain register storage */
    uint8_t fifo[FIFO_SIZE];
    int fifo_len;
    int fifo_pos;        /* next byte to read from the FIFO */

    /* state of the current SPI transaction */
    uint8_t spi_buf[1];  /* we move one byte at a time */
    bool first_byte;     /* next byte is the address byte */
    bool reading;
    uint8_t reg;         /* register selected by the address byte */

    bool halted;         /* card got HALT; only WUPA wakes it */
    uint32_t last_card;  /* to notice when the slider changes */
} chip_t;

/* ------------------------------------------------------------------ */
/* FIFO helpers                                                        */
/* ------------------------------------------------------------------ */
static void fifo_clear(chip_t *c) { c->fifo_len = 0; c->fifo_pos = 0; }

static void fifo_push(chip_t *c, uint8_t b)
{
    if (c->fifo_len < FIFO_SIZE) c->fifo[c->fifo_len++] = b;
}

/* CRC_A used by ISO 14443A (needed for SELECT and HALT frames) */
static uint16_t crc_a(const uint8_t *data, int len)
{
    uint16_t crc = 0x6363;
    for (int i = 0; i < len; i++) {
        uint8_t b = data[i] ^ (uint8_t)(crc & 0xFF);
        b ^= (uint8_t)(b << 4);
        crc = (uint16_t)((crc >> 8) ^ ((uint16_t)b << 8) ^ ((uint16_t)b << 3) ^ (b >> 4));
    }
    return crc;
}

static void soft_reset(chip_t *c)
{
    memset(c->regs, 0, sizeof(c->regs));
    c->regs[CommandReg] = 0x20;     /* reset values from the datasheet */
    c->regs[ComIrqReg]  = 0x14;
    c->regs[VersionReg] = 0x92;     /* "MFRC522 version 2.0" */
    fifo_clear(c);
}

/* ------------------------------------------------------------------ */
/* Pretend to be a card: read the request from the FIFO, write answer */
/* ------------------------------------------------------------------ */
static void transceive(chip_t *c)
{
    uint8_t req[FIFO_SIZE];
    int n = c->fifo_len;
    memcpy(req, c->fifo, (size_t)n);
    fifo_clear(c);

    /* which card is on the reader? (slider value) */
    float slider = attr_read_float(c->card_attr);
    uint32_t card = (slider > 0.0f) ? (uint32_t)(slider + 0.5f) : 0;
    if (card > 7) card = 0;
    if (card != c->last_card) {          /* new card or card removed */
        c->halted = false;
        c->last_card = card;
    }
    const uint8_t *uid = (card > 0) ? uids[card - 1] : NULL;
    bool answered = false;

    if (uid != NULL && n >= 1) {
        if (req[0] == PICC_WUPA || (req[0] == PICC_REQA && !c->halted)) {
            /* REQA/WUPA -> ATQA 0x04 0x00 ("4-byte UID card") */
            c->halted = false;
            fifo_push(c, 0x04);
            fifo_push(c, 0x00);
            answered = true;
        } else if (n >= 2 && req[0] == PICC_SEL_CL1 && req[1] == 0x20 && !c->halted) {
            /* ANTICOLLISION -> 4 UID bytes + BCC (XOR of the 4 bytes) */
            uint8_t bcc = 0;
            for (int i = 0; i < 4; i++) { fifo_push(c, uid[i]); bcc ^= uid[i]; }
            fifo_push(c, bcc);
            answered = true;
        } else if (n >= 2 && req[0] == PICC_SEL_CL1 && req[1] == 0x70 && !c->halted) {
            /* SELECT -> SAK 0x08 (MIFARE Classic 1K) + CRC */
            uint8_t sak = 0x08;
            uint16_t crc = crc_a(&sak, 1);
            fifo_push(c, sak);
            fifo_push(c, (uint8_t)(crc & 0xFF));
            fifo_push(c, (uint8_t)(crc >> 8));
            answered = true;
        } else if (req[0] == PICC_HALT) {
            c->halted = true;           /* a real card does not answer HALT */
        }
    }

    c->regs[ErrorReg] = 0;
    c->regs[ControlReg] = 0;            /* RxLastBits = 0: whole bytes */
    if (answered) {
        c->regs[ComIrqReg] |= IRQ_RX | IRQ_IDLE;
    } else {
        c->regs[ComIrqReg] |= IRQ_TIMER;   /* no answer: timer ran out */
    }
}

/* ------------------------------------------------------------------ */
/* Register reads and writes                                           */
/* ------------------------------------------------------------------ */
static uint8_t reg_read(chip_t *c, uint8_t reg)
{
    switch (reg) {
    case FIFODataReg:
        if (c->fifo_pos < c->fifo_len) return c->fifo[c->fifo_pos++];
        return 0;
    case FIFOLevelReg:
        return (uint8_t)(c->fifo_len - c->fifo_pos);
    case VersionReg:
        return 0x92;
    default:
        return c->regs[reg & 0x3F];
    }
}

static void reg_write(chip_t *c, uint8_t reg, uint8_t val)
{
    switch (reg) {
    case CommandReg:
        c->regs[CommandReg] = val;
        if ((val & 0x0F) == CMD_SOFTRESET) {
            soft_reset(c);
        } else if ((val & 0x0F) == CMD_CALCCRC) {
            /* CRC of the FIFO -> CRCResultReg, then DivIrqReg.CRCIRq */
            uint16_t crc = crc_a(c->fifo + c->fifo_pos, c->fifo_len - c->fifo_pos);
            c->regs[CRCResultRegL] = (uint8_t)(crc & 0xFF);
            c->regs[CRCResultRegH] = (uint8_t)(crc >> 8);
            c->regs[DivIrqReg] |= 0x04;
            fifo_clear(c);
        }
        break;
    case ComIrqReg:
    case DivIrqReg:
        /* bit 7 = Set1: 1 -> set the marked bits, 0 -> clear them */
        if (val & 0x80) c->regs[reg] |= (uint8_t)(val & 0x7F);
        else            c->regs[reg] &= (uint8_t)~(val & 0x7F);
        break;
    case FIFODataReg:
        if (c->fifo_pos > 0 && c->fifo_pos == c->fifo_len) fifo_clear(c);
        fifo_push(c, val);
        break;
    case FIFOLevelReg:
        if (val & 0x80) fifo_clear(c);      /* FlushBuffer */
        break;
    case BitFramingReg:
        c->regs[BitFramingReg] = val & 0x7F;
        /* StartSend while CommandReg = Transceive -> talk to the card */
        if ((val & 0x80) && (c->regs[CommandReg] & 0x0F) == CMD_TRANSCEIVE) {
            transceive(c);
        }
        break;
    case VersionReg:
        break;                               /* read only */
    default:
        c->regs[reg & 0x3F] = val;
        break;
    }
}

/* ------------------------------------------------------------------ */
/* SPI plumbing                                                        */
/* ------------------------------------------------------------------ */

/* Called after every byte. buffer[0] holds the byte the ESP32 sent. */
static void spi_done(void *user_data, uint8_t *buffer, uint32_t count)
{
    chip_t *c = (chip_t *)user_data;
    /* count 0: spi_stop() (CS went high) before a new byte arrived */
    if (count == 0) return;
    uint8_t in = buffer[0];
    uint8_t out = 0;

    if (c->first_byte) {
        /* address byte: bit 7 = read, bits 6..1 = register */
        c->first_byte = false;
        c->reading = (in & 0x80) != 0;
        c->reg = (in >> 1) & 0x3F;
        if (c->reading) out = reg_read(c, c->reg);
    } else if (c->reading) {
        /* in a read, each next byte is the next address (0x00 = end) */
        if (in & 0x80) out = reg_read(c, (in >> 1) & 0x3F);
    } else {
        reg_write(c, c->reg, in);
    }

    if (pin_read(c->cs) == LOW) {
        c->spi_buf[0] = out;                 /* sent during the next byte */
        spi_start(c->spi, c->spi_buf, 1);
    }
}

static void cs_changed(void *user_data, pin_t pin, uint32_t value)
{
    chip_t *c = (chip_t *)user_data;
    (void)pin;
    if (value == LOW) {
        /* selected: drive MISO and wait for the address byte */
        pin_mode(c->miso, OUTPUT);
        c->first_byte = true;
        c->spi_buf[0] = 0;
        spi_start(c->spi, c->spi_buf, 1);
    } else {
        /* not selected: stop and let go of MISO (the other reader shares it) */
        spi_stop(c->spi);
        pin_mode(c->miso, INPUT);
    }
}

void chip_init(void)
{
    chip_t *c = calloc(1, sizeof(chip_t));   /* one per reader in the diagram */

    c->cs   = pin_init("SDA", INPUT_PULLUP);
    c->miso = pin_init("MISO", INPUT);
    pin_init("RST", INPUT);
    pin_init("IRQ", INPUT);
    c->card_attr = attr_init_float("card", 0.0f);
    soft_reset(c);

    const spi_config_t spi_cfg = {
        .user_data = c,
        .sck  = pin_init("SCK", INPUT),
        .mosi = pin_init("MOSI", INPUT),
        .miso = c->miso,
        .mode = 0,
        .done = spi_done,
    };
    c->spi = spi_init(&spi_cfg);

    const pin_watch_config_t watch = {
        .user_data = c,
        .edge = BOTH,
        .pin_change = cs_changed,
    };
    pin_watch(c->cs, &watch);
}
