#pragma once
/* Images on the microSD card of the 2.8" touch LCD board: screenshots saved
 * on the LCD (SCANnnnn.BMP) and graphs saved from the web page (.png), all
 * in the card's root folder.
 *
 * The card shares the LCD's SPI bus, so only the LCD task touches it: a call
 * from another task is handed to the LCD task, which runs it in sd_service(),
 * and waits for the result (up to a few seconds), so the web page's requests
 * are made from a task of their own (main.cpp), never the web task. The
 * card is mounted while in use and released after a few idle seconds, so it
 * can be taken out at any other time. Builds without a card slot answer
 * SD_NO_CARD to everything. */
#include <Arduino.h>

enum SdResult { SD_OK = 0, SD_NO_CARD, SD_FAIL, SD_BUSY, SD_BAD_NAME };

struct SdEntry {
    char name[64];
    uint32_t size;
};

/* This build has a card slot. */
bool sd_available();
/* Image file names: 1-60 of A-Z a-z 0-9 . _ - ending in .png or .bmp. */
bool sd_valid_name(const char *name);

int sd_list(SdEntry *out, int max, int *count);
/* Reads up to `size` bytes at `offset`; *got is what was read, *total the
 * file's size. */
int sd_read(const char *name, uint32_t offset, uint8_t *buf, size_t size, size_t *got, uint32_t *total);
/* Writes at `offset`: 0 starts a new file (replacing one of that name),
 * anything else appends. */
int sd_write(const char *name, uint32_t offset, const uint8_t *data, size_t size);
int sd_rename(const char *from, const char *to);
int sd_remove(const char *name);
/* LCD task only: the next free SCANnnnn.BMP (SD_OK when `name` got one),
 * and writing a file in pieces (a screenshot). */
int sd_next_scan_name(char *name, size_t size);
int sd_create(const char *name);
bool sd_append(const uint8_t *data, size_t size);
bool sd_finish();

/* LCD task: run a request from another task, release the card when idle. */
void sd_service();
