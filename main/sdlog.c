#include "sdlog.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "board.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#define TAG "sdlog"

/* IDF tags that print several error lines when no card is inserted, a
   contact is bad or the card has no FAT volume. Muted during the mount;
   sdlog_append() logs one line with the error code instead. */
static const char *const s_mount_tags[] = {
    "sdmmc_common", "sdmmc_io", "sdmmc_sd", "sdmmc_req", "vfs_fat_sdmmc",
};
#define MOUNT_TAG_COUNT (sizeof s_mount_tags / sizeof s_mount_tags[0])

static esp_err_t mount_quietly(sdmmc_card_t **card)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    /* 1-bit bus. Every pin is set explicitly: the S3 default slot config
       uses GPIO12/13, which belong to the e-paper SPI bus. */
    sdmmc_slot_config_t slot = {
        .clk = BOARD_SD_CLK_GPIO,
        .cmd = BOARD_SD_CMD_GPIO,
        .d0 = BOARD_SD_D0_GPIO,
        .d1 = GPIO_NUM_NC,
        .d2 = GPIO_NUM_NC,
        .d3 = GPIO_NUM_NC,
        .d4 = GPIO_NUM_NC,
        .d5 = GPIO_NUM_NC,
        .d6 = GPIO_NUM_NC,
        .d7 = GPIO_NUM_NC,
        .cd = SDMMC_SLOT_NO_CD,
        .wp = SDMMC_SLOT_NO_WP,
        .width = 1,
        .flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP,
    };
    const esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 2,
    };

    esp_log_level_t levels[MOUNT_TAG_COUNT];
    for (size_t i = 0; i < MOUNT_TAG_COUNT; i++) {
        levels[i] = esp_log_level_get(s_mount_tags[i]);
        esp_log_level_set(s_mount_tags[i], ESP_LOG_NONE);
    }
    esp_err_t err = esp_vfs_fat_sdmmc_mount(SDLOG_MOUNT_POINT, &host, &slot, &mount_cfg, card);
    for (size_t i = 0; i < MOUNT_TAG_COUNT; i++) {
        esp_log_level_set(s_mount_tags[i], levels[i]);
    }
    return err;
}

/* Header if the file is new, then the row, pushed through to the card. */
/* True if `path` does not exist yet, is empty, or starts with SDLOG_HEADER. */
static bool header_matches(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return true;
    char first[sizeof(SDLOG_HEADER) + 1] = "";
    bool empty = fgets(first, sizeof first, f) == NULL;
    fclose(f);
    return empty || strncmp(first, SDLOG_HEADER, sizeof(SDLOG_HEADER) - 1) == 0;
}

static bool write_row(FILE *f, const char *row)
{
    if (fseek(f, 0, SEEK_END) != 0) return false;
    long size = ftell(f);
    if (size < 0) return false;
    if (size == 0 && fputs(SDLOG_HEADER "\n", f) == EOF) return false;
    if (fprintf(f, "%s\n", row) < 0) return false;
    if (fflush(f) != 0) return false;
    return fsync(fileno(f)) == 0;
}

bool sdlog_append(const reading_t *r, const sdlog_board_t *board, time_t now)
{
    char path[32];
    char row[128];
    if (!sdlog_month_path(path, sizeof path, now) || sdlog_format_row(row, sizeof row, r, board, now) < 0) {
        ESP_LOGW(TAG, "cannot format row for time %lld", (long long)now);
        return false;
    }

    sdmmc_card_t *card = NULL;
    esp_err_t err = mount_quietly(&card);
    if (err != ESP_OK) {
        /* A missing card times out on its first command. */
        const char *what = err == ESP_ERR_TIMEOUT ? "no SD card"
                         : err == ESP_FAIL ? "no FAT volume on SD card"
                         : "SD mount failed";
        ESP_LOGW(TAG, "%s (%s), row not logged", what, esp_err_to_name(err));
        return false;
    }

    /* A file written by an older firmware has fewer columns: continue in a
       second month file (8.3 name) instead of mixing formats. */
    if (!header_matches(path)) {
        size_t n = strlen(path);
        memmove(&path[n - 3], &path[n - 4], 5);   /* ".csv\0" one to the right */
        path[n - 4] = 'b';
    }

    FILE *f = fopen(path, "a");
    bool opened = f != NULL;
    bool ok = opened && write_row(f, row);
    if (opened && fclose(f) != 0) ok = false;
    int write_errno = errno;
    esp_vfs_fat_sdcard_unmount(SDLOG_MOUNT_POINT, card);

    if (!ok) {
        ESP_LOGW(TAG, "%s %s failed (errno %d)", opened ? "writing" : "opening", path, write_errno);
        return false;
    }
    ESP_LOGI(TAG, "%s: %s", path, row);
    return true;
}
