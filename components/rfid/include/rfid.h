#pragma once
/*
 * rfid - two MFRC522 RFID readers (ENTRY and EXIT).
 *
 * Use, from cocis_system/main/main.c:
 *     ESP_ERROR_CHECK(rfid_register_commands());   before entrance_start()
 *     ESP_ERROR_CHECK(rfid_start());               after entrance_start()
 *
 * rfid_start() sets up the pins and starts the "rfid" task. The task polls
 * both readers about every 50 ms; when a new card appears it calls
 * entrance_submit_scan(true/false, "01020304"), the same path as typing
 * "in <UID>" / "out <UID>". If a reader is missing the task keeps running
 * and the typed commands still work.
 *
 * Pins (software SPI, mode 0): SCK 16 and MOSI 17 shared by both readers;
 * ENTRY: CS 5, MISO 35; EXIT: CS 23, MISO 36 (VP).
 */
#include "esp_err.h"

/* Adds the "rfid" console command (reader status). Call before entrance_start(). */
esp_err_t rfid_register_commands(void);

/* Sets up the pins and both readers, then starts the rfid task. Call once. */
esp_err_t rfid_start(void);
