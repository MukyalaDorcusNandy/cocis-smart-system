/*
 * rfid.c - two MFRC522 RFID readers (ENTRY and EXIT).
 *
 * Hardware: two Wokwi "board-mfrc522" parts. In the simulator: click a
 * reader, pick a card (Blue, Green, ...) and press Tap (or switch on Hold).
 *
 * SPI is done in software ("bit-banging", SPI mode 0): the ESP32 drives the
 * clock (SCK) and data out (MOSI) pins itself and reads data in (MISO).
 *   SCK 16 and MOSI 17 are shared by both readers.
 *   Each reader has its OWN chip-select (SDA pin) and its OWN MISO line:
 *     ENTRY: CS GPIO 5,  MISO GPIO 35
 *     EXIT:  CS GPIO 23, MISO GPIO 36 (VP)
 *   With separate MISO lines the two readers can never talk over each other.
 *   RST is wired to 3.3 V (always running).
 *
 * Synchronisation: only the rfid task talks to the readers, one at a time.
 * The reader status shown by the "rfid" command is protected by state_lock.
 *
 * Flow:
 *   rfid_task --(about every 50 ms)--> read each reader --> new card?
 *       --> entrance_submit_scan(is_entry, uid) --> scan_queue --> access_task
 *
 * The small MFRC522 driver below only does what we need:
 *   init     - soft reset, timer, antenna on
 *   request  - "is a card there?" (REQA, then WUPA; the card answers ATQA)
 *   anticoll - read the 4-byte UID and check its BCC (XOR checksum)
 */
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "rom/ets_sys.h"
#include "esp_err.h"
#include "access_control.h"
#include "rfid.h"

/* ---- pins and settings (must match diagram.json) ---- */
#define PIN_SCK         16
#define PIN_MOSI        17
#define PIN_CS_ENTRY    5
#define PIN_MISO_ENTRY  35
#define PIN_CS_EXIT     23
#define PIN_MISO_EXIT   36
#define HALF_BIT_US     2       /* about 250 kHz SPI clock */

#define POLL_MS         50      /* pause between two rounds of polling */
#define GONE_POLLS      3       /* card counts as removed after 3 empty polls */

/* ---- MFRC522 registers and commands (datasheet section 9) ---- */
#define REG_COMMAND     0x01
#define REG_COMIRQ      0x04
#define REG_ERROR       0x06
#define REG_FIFO_DATA   0x09
#define REG_FIFO_LEVEL  0x0A
#define REG_BIT_FRAMING 0x0D
#define REG_CONTROL     0x0C
#define REG_COLL        0x0E
#define REG_MODE        0x11
#define REG_TX_MODE     0x12
#define REG_RX_MODE     0x13
#define REG_TX_CONTROL  0x14
#define REG_TX_ASK      0x15
#define REG_MOD_WIDTH   0x24
#define REG_T_MODE      0x2A
#define REG_T_PRESCALER 0x2B
#define REG_T_RELOAD_H  0x2C
#define REG_T_RELOAD_L  0x2D
#define REG_VERSION     0x37

#define CMD_IDLE        0x00
#define CMD_TRANSCEIVE  0x0C
#define CMD_SOFTRESET   0x0F

#define IRQ_TIMER       0x01    /* timeout: no card answered */
#define IRQ_RX          0x20    /* RxIRq: an answer was received */

#define PICC_REQA       0x26    /* "any new card here?" */
#define PICC_WUPA       0x52    /* wake up any card (also halted ones) */
#define PICC_SEL_CL1    0x93    /* anticollision / select, cascade level 1 */

/* One reader = its pins + its debounce state */
typedef struct {
    const char *name;        /* "ENTRY" / "EXIT" */
    bool        is_entry;
    int         cs_pin;
    int         miso_pin;
    bool        found;       /* VersionReg answered at start-up */
    uint8_t     version;     /* value read from VersionReg */
    char        last_uid[9]; /* card already reported ("" = none) */
    int         empty_polls; /* polls in a row with no card */
    int         cards_read;  /* how many cards this reader reported */
    /* diagnostics for the "rfid" command */
    int         answers;     /* REQA/WUPA answered (a card was seen) */
    int         uid_errors;  /* anticollision failed (no UID / bad BCC) */
    uint8_t     last_irq;    /* ComIrqReg after the last REQA */
    uint8_t     last_err;    /* ErrorReg after the last REQA */
} reader_t;

static reader_t readers[2] = {
    { .name = "ENTRY", .is_entry = true,  .cs_pin = PIN_CS_ENTRY, .miso_pin = PIN_MISO_ENTRY },
    { .name = "EXIT",  .is_entry = false, .cs_pin = PIN_CS_EXIT,  .miso_pin = PIN_MISO_EXIT  },
};

static SemaphoreHandle_t state_lock;   /* guards found/version/last_uid/cards_read */

/* ------------------------------------------------------------------ */
/* 1. REGISTER ACCESS OVER SOFTWARE SPI                                */
/*    address byte = (reg << 1) & 0x7E, with 0x80 added for a read     */
/* ------------------------------------------------------------------ */

/* Sends one byte and receives one byte at the same time (SPI mode 0:
 * data is set while SCK is low and read when SCK goes high). */
static uint8_t spi_byte(reader_t *r, uint8_t out)
{
    uint8_t in = 0;
    for (int bit = 7; bit >= 0; bit--) {
        gpio_set_level(PIN_MOSI, (out >> bit) & 1);
        ets_delay_us(HALF_BIT_US);
        gpio_set_level(PIN_SCK, 1);
        in = (uint8_t)((in << 1) | gpio_get_level(r->miso_pin));
        ets_delay_us(HALF_BIT_US);
        gpio_set_level(PIN_SCK, 0);
    }
    return in;
}

static void reg_write(reader_t *r, uint8_t reg, uint8_t val)
{
    gpio_set_level(r->cs_pin, 0);              /* select this reader */
    spi_byte(r, (uint8_t)((reg << 1) & 0x7E));
    spi_byte(r, val);
    gpio_set_level(r->cs_pin, 1);
}

static uint8_t reg_read(reader_t *r, uint8_t reg)
{
    gpio_set_level(r->cs_pin, 0);
    spi_byte(r, (uint8_t)(((reg << 1) & 0x7E) | 0x80));
    uint8_t v = spi_byte(r, 0x00);             /* value comes in the 2nd byte */
    gpio_set_level(r->cs_pin, 1);
    return v;
}

/* ------------------------------------------------------------------ */
/* 2. MFRC522 DRIVER                                                   */
/* ------------------------------------------------------------------ */

static void mfrc_init(reader_t *r)
{
    reg_write(r, REG_COMMAND, CMD_SOFTRESET);
    vTaskDelay(pdMS_TO_TICKS(50));
    /* same set-up as the common Arduino MFRC522 library */
    reg_write(r, REG_TX_MODE, 0x00);           /* 106 kbit/s, no CRC on send */
    reg_write(r, REG_RX_MODE, 0x00);
    reg_write(r, REG_MOD_WIDTH, 0x26);
    /* timer: about 25 ms timeout, so "no card" is detected quickly */
    reg_write(r, REG_T_MODE, 0x80);
    reg_write(r, REG_T_PRESCALER, 0xA9);
    reg_write(r, REG_T_RELOAD_H, 0x03);
    reg_write(r, REG_T_RELOAD_L, 0xE8);
    reg_write(r, REG_TX_ASK, 0x40);            /* 100% ASK modulation */
    reg_write(r, REG_MODE, 0x3D);              /* CRC preset 0x6363 */
    /* antenna on: TX1 and TX2 drivers */
    reg_write(r, REG_TX_CONTROL, reg_read(r, REG_TX_CONTROL) | 0x03);
}

/* Writes several bytes into the FIFO in one SPI transfer
 * (the address byte once, then the data bytes). */
static void fifo_write(reader_t *r, const uint8_t *data, int len)
{
    gpio_set_level(r->cs_pin, 0);
    spi_byte(r, (uint8_t)((REG_FIFO_DATA << 1) & 0x7E));
    for (int i = 0; i < len; i++) spi_byte(r, data[i]);
    gpio_set_level(r->cs_pin, 1);
}

/* Reads 'len' bytes from the FIFO in ONE SPI transfer, like the Arduino
 * MFRC522 library: the FIFO address is sent once per byte (and 0x00 at
 * the end); each answer byte comes back during the next address byte.
 * Reading the FIFO with separate transfers loses every second byte,
 * because the reader takes the next FIFO byte for every address byte. */
static void fifo_read(reader_t *r, uint8_t *back, int len)
{
    uint8_t addr = (uint8_t)(((REG_FIFO_DATA << 1) & 0x7E) | 0x80);
    gpio_set_level(r->cs_pin, 0);
    spi_byte(r, addr);
    for (int i = 0; i < len; i++) {
        back[i] = spi_byte(r, (i < len - 1) ? addr : 0x00);
    }
    gpio_set_level(r->cs_pin, 1);
}

/* Sends 'len' bytes to the card and reads its answer into 'back'.
 * tx_bits = bits of the last byte to send (0 = whole byte, 7 for REQA/WUPA).
 * Same steps as the Arduino MFRC522 library (PCD_CommunicateWithPICC).
 * Returns the number of bytes received, or -1 (no card / error). */
static int mfrc_transceive(reader_t *r, const uint8_t *data, int len,
                           uint8_t tx_bits, uint8_t *back, int back_max,
                           uint8_t *irq_out, uint8_t *err_out)
{
    reg_write(r, REG_COMMAND, CMD_IDLE);       /* stop anything running */
    reg_write(r, REG_COMIRQ, 0x7F);            /* clear all IRQ flags */
    reg_write(r, REG_FIFO_LEVEL, 0x80);        /* flush the FIFO */
    fifo_write(r, data, len);                  /* what to send to the card */
    reg_write(r, REG_BIT_FRAMING, tx_bits);    /* bits of the last byte */
    reg_write(r, REG_COMMAND, CMD_TRANSCEIVE);
    reg_write(r, REG_BIT_FRAMING, 0x80 | tx_bits);   /* StartSend */

    /* wait until the card answered (RxIRq) or the timer runs out
     * (TimerIRq = no card answered); at most about 60 ms. vTaskDelay
     * lets the other tasks run while we wait. */
    uint8_t irq = 0;
    bool done = false;
    for (int i = 0; i < 6; i++) {
        irq = reg_read(r, REG_COMIRQ);
        if (irq & IRQ_RX) { done = true; break; }   /* the card answered */
        if (irq & IRQ_TIMER) break;                  /* nobody answered */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (done) {
        /* give the reader time to put the whole answer into the FIFO */
        vTaskDelay(pdMS_TO_TICKS(10));
        irq = reg_read(r, REG_COMIRQ);
    }
    reg_write(r, REG_BIT_FRAMING, tx_bits);    /* clear StartSend */

    uint8_t err = reg_read(r, REG_ERROR);
    if (irq_out) *irq_out = irq;
    if (err_out) *err_out = err;
    if (!done) return -1;                      /* timeout: no card */
    if (err & 0x13) return -1;                 /* buffer overflow / parity / protocol error */

    int n = reg_read(r, REG_FIFO_LEVEL);
    if (n > back_max) n = back_max;
    if (n > 0) fifo_read(r, back, n);
    return n;
}

/* "Is a card there?" - REQA or WUPA (7 bits); a card answers ATQA (2 bytes) */
static bool mfrc_request(reader_t *r, uint8_t cmd)
{
    uint8_t atqa[2];
    int n = mfrc_transceive(r, &cmd, 1, 7, atqa, sizeof(atqa), &r->last_irq, &r->last_err);
    return n == 2;
}

/* Reads the 4-byte UID: the card sends UID0..UID3 + BCC (XOR of the four).
 * Returns false when no UID came back or the BCC check fails. */
static bool mfrc_anticoll(reader_t *r, uint8_t uid[4])
{
    uint8_t cmd[2] = { PICC_SEL_CL1, 0x20 };
    uint8_t back[8] = { 0 };
    uint8_t irq = 0, err = 0;
    /* ValuesAfterColl = 0: clear the bits after a collision */
    reg_write(r, REG_COLL, reg_read(r, REG_COLL) & 0x7F);
    int n = mfrc_transceive(r, cmd, 2, 0, back, sizeof(back), &irq, &err);

    bool ok = false;
    if (n >= 5) {
        uint8_t bcc = back[0] ^ back[1] ^ back[2] ^ back[3];
        ok = (bcc == back[4]);                 /* false = corrupted read */
    } else if (n == 4) {
        ok = true;                             /* UID without BCC byte */
    }
    if (ok) {
        memcpy(uid, back, 4);
    } else if (r->uid_errors % 50 == 0) {      /* first failure, then every 50th */
        printf("[RFID %s] UID read failed: %d bytes (%02X %02X %02X %02X %02X), ComIrq 0x%02X, Error 0x%02X\n",
               r->name, n, back[0], back[1], back[2], back[3], back[4], irq, err);
    }
    return ok;
}

/* Reads a card's UID as 8 upper-case hex chars ("01020304").
 * Returns false when there is no card. A 7-byte tag (the grey NFC tag,
 * 04:11:22:33:44:55:66) gives its first 4 bytes here, so it reads as
 * "04112233". It is not registered, so it is denied as an unknown tag. */
static bool read_card(reader_t *r, char out[9])
{
    uint8_t uid[4];
    /* REQA finds a new card. A card that was just read waits in another
     * state and only answers WUPA, so try WUPA too. */
    if (!mfrc_request(r, PICC_REQA) && !mfrc_request(r, PICC_WUPA)) return false;
    r->answers++;
    if (!mfrc_anticoll(r, uid)) {
        r->uid_errors++;
        return false;
    }
    snprintf(out, 9, "%02X%02X%02X%02X", uid[0], uid[1], uid[2], uid[3]);
    return true;
}

/* ------------------------------------------------------------------ */
/* 3. THE RFID TASK                                                    */
/* ------------------------------------------------------------------ */

/* Debounce: report a card once when it appears. Report it again only
 * after it has been removed (no card for GONE_POLLS polls in a row). */
static void poll_reader(reader_t *r)
{
    char uid[9];
    if (read_card(r, uid)) {
        r->empty_polls = 0;
        if (strcmp(uid, r->last_uid) != 0) {   /* a new card */
            xSemaphoreTake(state_lock, portMAX_DELAY);
            strcpy(r->last_uid, uid);
            r->cards_read++;
            xSemaphoreGive(state_lock);
            printf("[RFID %s] card %s\n", r->name, uid);
            if (entrance_submit_scan(r->is_entry, uid) != ESP_OK) {
                printf("[RFID %s] scan queue full, card ignored\n", r->name);
            }
        }
    } else if (r->last_uid[0] != '\0' && ++r->empty_polls >= GONE_POLLS) {
        xSemaphoreTake(state_lock, portMAX_DELAY);
        r->last_uid[0] = '\0';                 /* card removed */
        xSemaphoreGive(state_lock);
    }
}

static void rfid_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < 2; i++) {
        mfrc_init(&readers[i]);
        uint8_t v = reg_read(&readers[i], REG_VERSION);
        /* 0x91 / 0x92 = MFRC522 v1.0 / v2.0 (clones answer other values);
         * 0x00 or 0xFF means nothing answered on the MISO line */
        xSemaphoreTake(state_lock, portMAX_DELAY);
        readers[i].version = v;
        readers[i].found = (v != 0x00 && v != 0xFF);
        xSemaphoreGive(state_lock);
        if (readers[i].found) {
            printf("[RFID] %s reader v0x%02X OK\n", readers[i].name, v);
        } else {
            printf("[RFID] %s reader NOT FOUND (got 0x%02X), use typed in/out\n",
                   readers[i].name, v);
        }
    }

    while (1) {
        for (int i = 0; i < 2; i++) {
            if (readers[i].found) poll_reader(&readers[i]);
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

/* ------------------------------------------------------------------ */
/* 4. "rfid" COMMAND AND START-UP                                      */
/* ------------------------------------------------------------------ */

static void cmd_rfid(const char *args)
{
    (void)args;
    if (state_lock == NULL) { printf("rfid not started\n"); return; }
    xSemaphoreTake(state_lock, portMAX_DELAY);
    for (int i = 0; i < 2; i++) {
        reader_t *r = &readers[i];
        printf("  %-5s reader: %s (version 0x%02X), cards read %d, card on reader: %s\n",
               r->name, r->found ? "OK" : "NOT FOUND", r->version, r->cards_read,
               r->last_uid[0] ? r->last_uid : "none");
        printf("        card answers %d, UID errors %d, last ComIrq 0x%02X, last Error 0x%02X\n",
               r->answers, r->uid_errors, r->last_irq, r->last_err);
    }
    xSemaphoreGive(state_lock);
    printf("  tap a card: click a reader, pick a card, press Tap (or type in/out <UID>)\n");
}

esp_err_t rfid_register_commands(void)
{
    return entrance_register_command("rfid", "rfid                status of the two RFID readers", cmd_rfid);
}

esp_err_t rfid_start(void)
{
    state_lock = xSemaphoreCreateMutex();
    if (state_lock == NULL) return ESP_ERR_NO_MEM;

    /* outputs: clock, data out and the two chip-selects (idle high = not selected) */
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_SCK) | (1ULL << PIN_MOSI) |
                        (1ULL << PIN_CS_ENTRY) | (1ULL << PIN_CS_EXIT),
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t e = gpio_config(&out);
    if (e != ESP_OK) return e;
    gpio_set_level(PIN_SCK, 0);
    gpio_set_level(PIN_CS_ENTRY, 1);
    gpio_set_level(PIN_CS_EXIT, 1);

    /* inputs: one MISO line per reader (GPIO 35 and 36 are input-only pins) */
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_MISO_ENTRY) | (1ULL << PIN_MISO_EXIT),
        .mode = GPIO_MODE_INPUT,
    };
    e = gpio_config(&in);
    if (e != ESP_OK) return e;

    if (xTaskCreate(rfid_task, "rfid", 4096, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
