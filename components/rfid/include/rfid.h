#pragma once
/*
 * rfid - two MFRC522 RFID readers (ENTRY and EXIT) on one SPI bus.
 *
 * Use, from cocis_system/main/main.c, after entrance_start():
 *     ESP_ERROR_CHECK(rfid_start());
 *
 * rfid_start() sets up SPI3 and starts the "rfid" task. The task polls both
 * readers every 150 ms; when a new card appears it calls
 * entrance_submit_scan(true/false, "01020304"), the same path as typing
 * "in <UID>" / "out <UID>". If a reader is missing the task keeps running
 * and the typed commands still work.
 *
 * Pins (SPI3_HOST): SCK 16, MOSI 17, MISO 35, CS entry 5, CS exit 23.
 */
#include "esp_err.h"

/* Sets up the SPI bus and both readers, then starts the rfid task. Call once. */
esp_err_t rfid_start(void);
