/*
 * main.cpp — SimCoupe ESP32-P4-PC entry point (stub)
 *
 * Boot sequence (final):
 *   1. NVS init (app_main, LP DRAM stack)
 *   2. Spawn simcoupe_task (PSRAM stack, 256 KB)
 *   3. HDMI init (LT8912B via MIPI DSI)
 *   4. Audio init (ES8311 via I2S + shared I2C bus)
 *   5. USB HID keyboard init
 *   6. SD card mount
 *   7. SimCoupe Main::Init()
 *   8. Emulation loop (50 Hz PAL)
 *
 * This file is currently a stub that verifies the ESP-IDF project
 * structure compiles correctly before the full port is implemented.
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "simcoupe";

/* 256 KB PSRAM stack for the main emulation task.
 * CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y lets xTaskCreate allocate
 * from PSRAM automatically when size > CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL. */
#define SIMCOUPE_TASK_STACK_KB  256

static void simcoupe_task(void *arg)
{
    ESP_LOGI(TAG, "SimCoupe task started (stub)");

    /* TODO (T08): Full boot sequence:
     *   sim_display_init()
     *   sim_audio_init_with_bus(sim_display_get_i2c_bus())
     *   sim_kbd_init(kbd_queue)
     *   sim_sdcard_mount()
     *   Main::Init(argc, argv)
     *   emulation_loop()
     */

    while (1) {
        ESP_LOGI(TAG, "SimCoupe stub running — full implementation pending");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

extern "C" void app_main(void)
{
    /* NVS init — must run on LP DRAM stack (app_main default) */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_err = nvs_flash_init();
    }
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: 0x%x", nvs_err);
    }

    ESP_LOGI(TAG, "SimCoupe ESP32-P4-PC starting...");

    /* Spawn main emulation task with large PSRAM stack */
    xTaskCreate(simcoupe_task, "simcoupe",
                SIMCOUPE_TASK_STACK_KB * 1024,
                NULL, 5, NULL);
}
