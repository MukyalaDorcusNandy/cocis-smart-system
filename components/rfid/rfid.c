/*
 * rfid.c - two MFRC522 RFID readers (ENTRY and EXIT).
 *
 * Hardware: two Wokwi "board-mfrc522" parts. Both readers share one SPI bus (SPI3_HOST: SCK 16, MOSI 17,
 * MISO 35). Each reader has its own chip-select pin (SDA on the MFRC522):
 * GPIO 5 = ENTRY, GPIO 23 = EXIT. RST is wired to 3.3 V (always running).
 * In the simulator: click a reader, pick a card (Blue, Green, ...) and press
 * Tap. The ESP-IDF SPI driver gives each reader
 * a "device handle" and only lets one transaction use the bus at a time,
 * so the two readers can never talk over each other (synchronisation).
 *
 * Flow:
 *   rfid_task --(every 150 ms)--> read each reader --> new card?
 *       --> entrance_submit_scan(is_entry, uid) --> scan_queue --> access_task
 *
 * The small MFRC522 driver below only does what we need:
 *   init     - soft reset, timer, antenna on
 *   request  - "is a card there?" (WUPA, the card answers ATQA)
 *   anticoll - read the 4-byte UID and check its BCC (XOR checksum)
 */
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "access_control.h"
#include "rfid.h"

/* ---- pins and settings (must match diagram.json) ---- */
#define RFID_HOST       SPI3_HOST
#define PIN_SCK         16
#define PIN_MOSI        17
#define PIN_MISO        35
#define PIN_CS_ENTRY    5
#define PIN_CS_EXIT     23
#define SPI_CLOCK_HZ    (1 * 1000 * 1000)   /* 1 MHz */

#define POLL_MS         150     /* how often each reader is checked */
#define GONE_POLLS      2       /* card counts as removed after 2 empty polls */

/* ---- MFRC522 registers and commands (datasheet section 9) ---- */
#define REG_COMMAND     0x01
#define REG_COMIRQ      0x04
#define REG_ERROR       0x06
#define REG_FIFO_DATA   0x09
#define REG_FIFO_LEVEL  0x0A
#define REG_BIT_FRAMING 0x0D
#define REG_MODE        0x11
#define REG_TX_CONTROL  0x14
#define REG_TX_ASK      0x15
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
#define IRQ_RX_IDLE     0x30    /* RxIRq | IdleIRq: transceive finished */

#define PICC_WUPA       0x52    /* wake up any card (also halted ones) */
#define PICC_SEL_CL1    0x93    /* anticollision / select, cascade level 1 */

/* One reader = one SPI device + its debounce state */
typedef struct {
    const char         *name;       /* "ENTRY" / "EXIT" */
    bool                is_entry;
    int                 cs_pin;
    spi_device_handle_t dev;
    bool                found;      /* VersionReg answered at start-up */
    char                last_uid[9];/* card already reported ("" = none) */
    int                 empty_polls;/* polls in a row with no card */
} reader_t;

static reader_t readers[2] = {
    { .name = "ENTRY", .is_entry = true,  .cs_pin = PIN_CS_ENTRY },
    { .name = "EXIT",  .is_entry = false, .cs_pin = PIN_CS_EXIT  },
};

/* ------------------------------------------------------------------ */
/* 1. REGISTER ACCESS OVER SPI                                         */
/*    address byte = (reg << 1) & 0x7E, with 0x80 added for a read     */
/* ------------------------------------------------------------------ */

static esp_err_t reg_write(reader_t *r, uint8_t reg, uint8_t val)
{
    spi_transaction_t t = {
        .flags = SPI_TRANS_USE_TXDATA,
        .length = 16,                          /* 2 bytes, in bits */
        .tx_data = { (uint8_t)((reg << 1) & 0x7E), val },
    };
    return spi_device_polling_transmit(r->dev, &t);
}

static uint8_t reg_read(reader_t *r, uint8_t reg)
{
    spi_transaction_t t = {
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .length = 16,
        .tx_data = { (uint8_t)(((reg << 1) & 0x7E) | 0x80), 0x00 },
    };
    if (spi_device_polling_transmit(r->dev, &t) != ESP_OK) return 0;
    return t.rx_data[1];                       /* value comes in the 2nd byte */
}

/* ------------------------------------------------------------------ */
/* 2. MFRC522 DRIVER                                                   */
/* ------------------------------------------------------------------ */

static void mfrc_init(reader_t *r)
{
    reg_write(r, REG_COMMAND, CMD_SOFTRESET);
    vTaskDelay(pdMS_TO_TICKS(50));
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

/* Sends 'len' bytes to the card and reads its answer into 'back'.
 * tx_bits = bits of the last byte to send (0 = whole byte, 7 for WUPA).
 * Returns the number of bytes received, or -1 (no card / error). */
static int mfrc_transceive(reader_t *r, const uint8_t *data, int len,
                           uint8_t tx_bits, uint8_t *back, int back_max)
{
    reg_write(r, REG_COMMAND, CMD_IDLE);       /* stop anything running */
    reg_write(r, REG_COMIRQ, 0x7F);            /* clear all IRQ flags */
    reg_write(r, REG_FIFO_LEVEL, 0x80);        /* flush the FIFO */
    for (int i = 0; i < len; i++) {
        reg_write(r, REG_FIFO_DATA, data[i]);
    }
    reg_write(r, REG_COMMAND, CMD_TRANSCEIVE);
    reg_write(r, REG_BIT_FRAMING, 0x80 | tx_bits);   /* StartSend */

    /* wait for "answer received" or "timeout" (at most ~20 ticks) */
    uint8_t irq = 0;
    for (int i = 0; i < 20; i++) {
        irq = reg_read(r, REG_COMIRQ);
        if (irq & (IRQ_RX_IDLE | IRQ_TIMER)) break;
        vTaskDelay(1);
    }
    reg_write(r, REG_BIT_FRAMING, tx_bits);    /* clear StartSend */

    if (!(irq & IRQ_RX)) return -1;            /* nothing received: no card */
    if (reg_read(r, REG_ERROR) & 0x1B) return -1;   /* buffer/parity/CRC/protocol error */

    int n = reg_read(r, REG_FIFO_LEVEL);
    if (n > back_max) n = back_max;
    for (int i = 0; i < n; i++) {
        back[i] = reg_read(r, REG_FIFO_DATA);
    }
    return n;
}

/* "Is a card there?" - WUPA (7 bits), a card answers with 2 bytes (ATQA) */
static bool mfrc_request(reader_t *r)
{
    uint8_t cmd = PICC_WUPA;
    uint8_t atqa[2];
    return mfrc_transceive(r, &cmd, 1, 7, atqa, sizeof(atqa)) == 2;
}

/* Reads the 4-byte UID: the card sends UID0..UID3 + BCC (XOR of the four) */
static bool mfrc_anticoll(reader_t *r, uint8_t uid[4])
{
    uint8_t cmd[2] = { PICC_SEL_CL1, 0x20 };
    uint8_t back[5];
    if (mfrc_transceive(r, cmd, 2, 0, back, sizeof(back)) != 5) return false;
    uint8_t bcc = back[0] ^ back[1] ^ back[2] ^ back[3];
    if (bcc != back[4]) return false;          /* corrupted read */
    memcpy(uid, back, 4);
    return true;
}

/* Reads a card's UID as 8 upper-case hex chars ("01020304").
 * Returns false when there is no card. A 7-byte tag (the grey NFC tag)
 * answers 0x88 (cascade tag) + its first 3 bytes here; we keep those 4 bytes
 * as its ID. It is not registered, so it is denied as an unknown tag. */
static bool read_card(reader_t *r, char out[9])
{
    uint8_t uid[4];
    /* a real card that was just read may ignore one WUPA, so try twice */
    if (!mfrc_request(r) && !mfrc_request(r)) return false;
    if (!mfrc_anticoll(r, uid)) return false;
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
            strcpy(r->last_uid, uid);
            printf("[RFID %s] card %s\n", r->name, uid);
            if (entrance_submit_scan(r->is_entry, uid) != ESP_OK) {
                printf("[RFID %s] scan queue full, card ignored\n", r->name);
            }
        }
    } else if (r->last_uid[0] != '\0' && ++r->empty_polls >= GONE_POLLS) {
        r->last_uid[0] = '\0';                 /* card removed */
    }
}

static void rfid_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < 2; i++) {
        mfrc_init(&readers[i]);
        uint8_t v = reg_read(&readers[i], REG_VERSION);
        /* 0x91 / 0x92 = MFRC522 v1.0 / v2.0 (clones answer other values);
         * 0x00 or 0xFF means nothing answered on the SPI bus */
        readers[i].found = (v != 0x00 && v != 0xFF);
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
/* 4. START-UP                                                         */
/* ------------------------------------------------------------------ */

esp_err_t rfid_start(void)
{
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };
    esp_err_t e = spi_bus_initialize(RFID_HOST, &bus, SPI_DMA_DISABLED);
    if (e != ESP_OK) return e;

    for (int i = 0; i < 2 && e == ESP_OK; i++) {
        spi_device_interface_config_t dev = {
            .clock_speed_hz = SPI_CLOCK_HZ,
            .mode = 0,                         /* MFRC522 uses SPI mode 0 */
            .spics_io_num = readers[i].cs_pin,
            .queue_size = 1,
        };
        e = spi_bus_add_device(RFID_HOST, &dev, &readers[i].dev);
    }
    if (e != ESP_OK) return e;

    if (xTaskCreate(rfid_task, "rfid", 4096, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
