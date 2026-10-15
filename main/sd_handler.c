// main/sd_handler.c
#include "sd_handler.h"
#include "config.h"
#include "esp_log.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_vfs_fat.h"
#include "pins.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

static const char* TAG = "sd_handler";
static bool sd_initialized = false;
static sdmmc_card_t* sd_card = NULL;

void init_sd_handler(void)
{
    if (sd_initialized) {
        ESP_LOGW(TAG, "SD handler already initialized");
        return;
    }

    const app_config_t* cfg = get_config();
    if (!cfg->sd_enable) {
        ESP_LOGW(TAG, "SD disabled; skipping init");
        return;
    }

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    // GPIO12/GPIO13 are the native USB D-/D+ lines; they cannot be driven as
    // SDSPI MISO/SCLK while the USB Serial/JTAG console is active.
    if (cfg->pins.sdspi_miso == GPIO_NUM_12 || cfg->pins.sdspi_miso == GPIO_NUM_13 ||
        cfg->pins.sdspi_sclk == GPIO_NUM_12 || cfg->pins.sdspi_sclk == GPIO_NUM_13) {
        ESP_LOGW(TAG, "SDSPI MISO/SCLK conflict with USB Serial/JTAG; SD disabled");
        return;
    }
#endif

    // Initialize the SPI bus that SDSPI rides on. MOSI/MISO/SCLK belong to the
    // bus config (not the device config); CS is set on the device.
    spi_bus_config_t buscfg = {
        .mosi_io_num = cfg->pins.sdspi_mosi,
        .miso_io_num = cfg->pins.sdspi_miso,
        .sclk_io_num = cfg->pins.sdspi_sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4092,
    };
    esp_err_t bus_ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (bus_ret != ESP_OK && bus_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(bus_ret));
        return;
    }

    // Configure SDSPI host
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    host.flags = SDMMC_HOST_FLAG_SPI | SDMMC_HOST_FLAG_DEINIT_ARG;
    host.deinit_p = &sdspi_host_remove_device;

    // Configure device
    sdspi_device_config_t dev_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev_cfg.gpio_cs = cfg->pins.sdspi_cs; // Override default CS pin

    // Initialize FAT filesystem
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    // Mount the filesystem - let esp_vfs_fat_sdspi_mount do the device initialization
    sdmmc_card_t *card = NULL;
    esp_err_t ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &dev_cfg, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount filesystem: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "SD card initialized successfully");
    sd_card = card;
    sd_initialized = true;
}

sd_status_t get_sd_status(void)
{
    if (!sd_initialized) {
        return SD_STATUS_UNKNOWN;
    }

    // Simple check - in a real implementation we would check card status
    return SD_STATUS_READY;
}

static int sd_write_impl(const char* filename, const uint8_t* data, size_t len, bool verbose)
{
    if (!sd_initialized || !filename || !data || len == 0) {
        return -1;
    }

    char full_path[256];
    snprintf(full_path, sizeof(full_path), "/sdcard/%s", filename);

    FILE* file = fopen(full_path, "ab");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", full_path);
        return -1;
    }

    size_t written = fwrite(data, 1, len, file);
    fclose(file);

    if (written != len) {
        ESP_LOGE(TAG, "Failed to write all data to file");
        return -1;
    }

    if (verbose) {
        ESP_LOGI(TAG, "Successfully wrote %d bytes to %s", (int)len, filename);
    }
    return 0;
}

int sd_write_data(const char* filename, const uint8_t* data, size_t len)
{
    return sd_write_impl(filename, data, len, true);
}

int sd_write_data_quiet(const char* filename, const uint8_t* data, size_t len)
{
    return sd_write_impl(filename, data, len, false);
}

int sd_write_rotate(const char* filename, const uint8_t* data, size_t len)
{
    if (!sd_initialized || !filename || !data || len == 0) {
        return -1;
    }

    char full[256];
    snprintf(full, sizeof(full), "/sdcard/%s", filename);

    // Rotate before appending when this write would push the file past the cap.
    struct stat st;
    if (stat(full, &st) == 0 && (size_t)st.st_size + len > LOG_ROTATE_MAX_SIZE) {
        bool rot_ok = true;

        // Shift copies: .9 -> .10, .8 -> .9, ... .1 -> .2. FatFs f_rename()
        // fails with EEXIST when the destination exists (it does not overwrite),
        // so clear each destination first; ENOENT means there was nothing to
        // remove. A rename that fails for any other reason aborts the rotation
        // so the file cannot grow past the cap and history is not silently lost.
        for (int i = LOG_ROTATE_DEPTH - 1; i >= 1 && rot_ok; i--) {
            char src[288];
            char dst[288];
            snprintf(src, sizeof(src), "/sdcard/%s.%d", filename, i);
            snprintf(dst, sizeof(dst), "/sdcard/%s.%d", filename, i + 1);
            if (unlink(dst) != 0 && errno != ENOENT) {
                rot_ok = false;
                break;
            }
            if (rename(src, dst) != 0 && errno != ENOENT) {
                rot_ok = false;  // src exists but could not be moved
            }
        }

        // Move the current file out of the way.
        if (rot_ok) {
            char bak[288];
            snprintf(bak, sizeof(bak), "/sdcard/%s.1", filename);
            if (unlink(bak) != 0 && errno != ENOENT) {
                rot_ok = false;
            } else if (rename(full, bak) != 0) {
                rot_ok = false;
            }
        }

        if (!rot_ok) {
            ESP_LOGE(TAG, "sd_write_rotate: rename chain failed for %s", filename);
            return -1;  // refuse to append past the cap
        }
    }

    // Append to the (possibly new) current file.
    FILE* file = fopen(full, "ab");
    if (!file) {
        ESP_LOGE(TAG, "sd_write_rotate: fopen(%s) failed", filename);
        return -1;
    }
    size_t written = fwrite(data, 1, len, file);
    fclose(file);
    if (written != len) {
        ESP_LOGE(TAG, "sd_write_rotate: partial write to %s (%u/%u)",
                 filename, (unsigned)written, (unsigned)len);
        return -1;
    }
    return 0;
}

int sd_read_data(const char* filename, uint8_t* buffer, size_t buffer_size)
{
    if (!sd_initialized || !filename || !buffer || buffer_size == 0) {
        return -1;
    }

    char full_path[256];
    snprintf(full_path, sizeof(full_path), "/sdcard/%s", filename);

    FILE* file = fopen(full_path, "rb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open file for reading: %s", full_path);
        return -1;
    }

    size_t read = fread(buffer, 1, buffer_size, file);
    fclose(file);

    ESP_LOGI(TAG, "Successfully read %d bytes from %s", (int)read, filename);
    return (int)read;
}

int sd_list_files(char* buffer, size_t buffer_size)
{
    if (!sd_initialized || !buffer || buffer_size == 0) {
        return -1;
    }

    // In a real implementation, this would list files in the directory
    // For now, we'll return a simple response
    snprintf(buffer, buffer_size, "{\"files\":[]}");
    return 0;
}

void sd_cleanup(void)
{
    if (sd_initialized) {
        esp_vfs_fat_sdcard_unmount("/sdcard", sd_card);
        sd_card = NULL;
        sd_initialized = false;
        ESP_LOGI(TAG, "SD card cleaned up");
    }
}