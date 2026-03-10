/*
 * esp_lcd_lt8912b.c — Lontium LT8912B MIPI DSI → HDMI bridge driver
 *
 * Adapted from esp32-mos for SimCoupe: uses CONFIG_SIM_DISPLAY_* Kconfig symbols.
 *
 * All timing parameters come from Kconfig (single source of truth):
 *   CONFIG_SIM_DISPLAY_HACT/VACT, PCLK_MHZ, HS/HFP/HBP, VS/VFP/VBP,
 *   HSYNC_POL/VSYNC_POL, SETTLE, DDS_0/1/2, DSI_LANE_MBPS.
 * Board-specific values live in sdkconfig.defaults.olimex-p4pc.
 *
 * LT8912B I2C sub-addresses (all on the same bus):
 *   0x48 — main  (digital/analog init, HPD status, output path)
 *   0x49 — MIPI/DDS  (DSI config, video timing, DDS clock)
 *   0x4A — audio/AVI (unused for now)
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"

#include "esp_lcd_lt8912b.h"

static const char *TAG = "lt8912b";

/* ------------------------------------------------------------------ */
/* LT8912B I2C addresses                                               */
/* ------------------------------------------------------------------ */
#define LT8912B_ADDR_MAIN       0x48
#define LT8912B_ADDR_CEC_DSI    0x49
#define LT8912B_ADDR_AUDIO      0x4A

#define LT8912B_REG_CHIP_ID_H   0x00
#define LT8912B_REG_CHIP_ID_L   0x01
#define LT8912B_CHIP_ID_H       0x12
#define LT8912B_CHIP_ID_L       0xB2
#define LT8912B_REG_HPD_STATUS  0xC1

/* ------------------------------------------------------------------ */
/* Driver state                                                         */
/* ------------------------------------------------------------------ */
typedef struct {
    i2c_master_bus_handle_t  bus;
    i2c_master_dev_handle_t  dev_main;
    i2c_master_dev_handle_t  dev_cec_dsi;
    i2c_master_dev_handle_t  dev_audio;
    int                      hpd_gpio;
    bool                     initialized;
    bool                     bus_owned;
} lt8912b_t;

static lt8912b_t s_lt = { 0 };

/* ------------------------------------------------------------------ */
/* I2C helpers                                                          */
/* ------------------------------------------------------------------ */
static esp_err_t lt_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, sizeof(buf), 100);
}

static esp_err_t lt_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(dev, &reg, 1, val, 1, 100);
}

/* ------------------------------------------------------------------ */
/* Register sequences                                                   */
/* ------------------------------------------------------------------ */
static esp_err_t lt8912b_write_init_config(void)
{
    i2c_master_dev_handle_t m = s_lt.dev_main;

    ESP_RETURN_ON_ERROR(lt_write(m, 0x02, 0xF7), TAG, "init 0x02 pll_rst");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x02, 0xFF), TAG, "init 0x02 pll_rel");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x08, 0xFF), TAG, "init 0x08");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x09, 0xFF), TAG, "init 0x09");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x0A, 0xFF), TAG, "init 0x0A");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x0B, 0x7C), TAG, "init 0x0B");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x0C, 0xFF), TAG, "init 0x0C");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x31, 0xE1), TAG, "init 0x31");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x32, 0xE1), TAG, "init 0x32");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x33, 0x0C), TAG, "init 0x33");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x37, 0x00), TAG, "init 0x37");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x38, 0x22), TAG, "init 0x38");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x60, 0x82), TAG, "init 0x60");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x39, 0x45), TAG, "init 0x39");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x3A, 0x00), TAG, "init 0x3A");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x3B, 0x00), TAG, "init 0x3B");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x44, 0x31), TAG, "init 0x44");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x55, 0x44), TAG, "init 0x55");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x57, 0x01), TAG, "init 0x57");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x5A, 0x02), TAG, "init 0x5A");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x3E, 0xD6), TAG, "init 0x3E");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x3F, 0xD4), TAG, "init 0x3F");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x41, 0x3C), TAG, "init 0x41");

    return ESP_OK;
}

static esp_err_t lt8912b_write_mipi_basic(void)
{
    i2c_master_dev_handle_t d = s_lt.dev_cec_dsi;

    ESP_RETURN_ON_ERROR(lt_write(d, 0x10, 0x01), TAG, "mipi 0x10");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x11, CONFIG_SIM_DISPLAY_SETTLE), TAG, "mipi settle");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x12, 0x04), TAG, "mipi 0x12");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x13, 0x02), TAG, "mipi 0x13");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x14, 0x00), TAG, "mipi 0x14");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x15, 0x00), TAG, "mipi 0x15");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x1A, 0x03), TAG, "mipi 0x1A");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x1B, 0x03), TAG, "mipi 0x1B");

    return ESP_OK;
}

static esp_err_t lt8912b_write_video_timing(void)
{
    i2c_master_dev_handle_t d = s_lt.dev_cec_dsi;

#define HTOTAL (CONFIG_SIM_DISPLAY_HACT + CONFIG_SIM_DISPLAY_HS + CONFIG_SIM_DISPLAY_HFP + CONFIG_SIM_DISPLAY_HBP)
#define VTOTAL (CONFIG_SIM_DISPLAY_VACT + CONFIG_SIM_DISPLAY_VS + CONFIG_SIM_DISPLAY_VFP + CONFIG_SIM_DISPLAY_VBP)
#define SYNC_POL ((CONFIG_SIM_DISPLAY_VSYNC_POL & 1) | ((CONFIG_SIM_DISPLAY_HSYNC_POL & 1) << 1))

    ESP_LOGI(TAG, "Video timing: %dx%d htotal=%d vtotal=%d pclk=%dMHz pol=0x%02X",
             CONFIG_SIM_DISPLAY_HACT, CONFIG_SIM_DISPLAY_VACT, HTOTAL, VTOTAL,
             CONFIG_SIM_DISPLAY_PCLK_MHZ, SYNC_POL);

    ESP_RETURN_ON_ERROR(lt_write(d, 0x18, CONFIG_SIM_DISPLAY_HS),              TAG, "vt hs");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x19, CONFIG_SIM_DISPLAY_VS),              TAG, "vt vs");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x1C, CONFIG_SIM_DISPLAY_HACT & 0xFF),    TAG, "vt hact_l");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x1D, CONFIG_SIM_DISPLAY_HACT >> 8),      TAG, "vt hact_h");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x2F, 0x0C),                               TAG, "vt fifo");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x34, HTOTAL & 0xFF),                      TAG, "vt htot_l");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x35, HTOTAL >> 8),                        TAG, "vt htot_h");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x36, VTOTAL & 0xFF),                      TAG, "vt vtot_l");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x37, VTOTAL >> 8),                        TAG, "vt vtot_h");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x38, CONFIG_SIM_DISPLAY_VBP & 0xFF),     TAG, "vt vbp_l");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x39, CONFIG_SIM_DISPLAY_VBP >> 8),       TAG, "vt vbp_h");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x3A, CONFIG_SIM_DISPLAY_VFP & 0xFF),     TAG, "vt vfp_l");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x3B, CONFIG_SIM_DISPLAY_VFP >> 8),       TAG, "vt vfp_h");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x3C, CONFIG_SIM_DISPLAY_HBP & 0xFF),     TAG, "vt hbp_l");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x3D, CONFIG_SIM_DISPLAY_HBP >> 8),       TAG, "vt hbp_h");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x3E, CONFIG_SIM_DISPLAY_HFP & 0xFF),     TAG, "vt hfp_l");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x3F, CONFIG_SIM_DISPLAY_HFP >> 8),       TAG, "vt hfp_h");
    ESP_RETURN_ON_ERROR(lt_write(s_lt.dev_main, 0xAB, SYNC_POL),              TAG, "vt pol");

#undef HTOTAL
#undef VTOTAL
#undef SYNC_POL

    return ESP_OK;
}

static esp_err_t lt8912b_write_dds_config(void)
{
    i2c_master_dev_handle_t d = s_lt.dev_cec_dsi;

    ESP_RETURN_ON_ERROR(lt_write(d, 0x4E, CONFIG_SIM_DISPLAY_DDS_0), TAG, "dds 4E");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x4F, CONFIG_SIM_DISPLAY_DDS_1), TAG, "dds 4F");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x50, CONFIG_SIM_DISPLAY_DDS_2), TAG, "dds 50");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x51, 0x80), TAG, "dds 51 arm");

    ESP_RETURN_ON_ERROR(lt_write(d, 0x1F, 0x5E), TAG, "dds 1F");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x20, 0x01), TAG, "dds 20");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x21, 0x2C), TAG, "dds 21");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x22, 0x01), TAG, "dds 22");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x23, 0xFA), TAG, "dds 23");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x24, 0x00), TAG, "dds 24");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x25, 0xC8), TAG, "dds 25");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x26, 0x00), TAG, "dds 26");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x27, 0x5E), TAG, "dds 27");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x28, 0x01), TAG, "dds 28");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x29, 0x2C), TAG, "dds 29");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x2A, 0x01), TAG, "dds 2A");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x2B, 0xFA), TAG, "dds 2B");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x2C, 0x00), TAG, "dds 2C");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x2D, 0xC8), TAG, "dds 2D");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x2E, 0x00), TAG, "dds 2E");

    ESP_RETURN_ON_ERROR(lt_write(d, 0x42, 0x64), TAG, "dds 42");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x43, 0x00), TAG, "dds 43");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x44, 0x04), TAG, "dds 44");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x45, 0x00), TAG, "dds 45");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x46, 0x59), TAG, "dds 46");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x47, 0x00), TAG, "dds 47");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x48, 0xF2), TAG, "dds 48");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x49, 0x06), TAG, "dds 49");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x4A, 0x00), TAG, "dds 4A");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x4B, 0x72), TAG, "dds 4B");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x4C, 0x45), TAG, "dds 4C");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x4D, 0x00), TAG, "dds 4D");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x52, 0x08), TAG, "dds 52");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x53, 0x00), TAG, "dds 53");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x54, 0xB2), TAG, "dds 54");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x55, 0x00), TAG, "dds 55");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x56, 0xE4), TAG, "dds 56");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x57, 0x0D), TAG, "dds 57");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x58, 0x00), TAG, "dds 58");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x59, 0xE4), TAG, "dds 59");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x5A, 0x8A), TAG, "dds 5A");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x5B, 0x00), TAG, "dds 5B");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x5C, 0x34), TAG, "dds 5C");

    ESP_RETURN_ON_ERROR(lt_write(d, 0x1E, 0x4F), TAG, "dds 1E");
    ESP_RETURN_ON_ERROR(lt_write(d, 0x51, 0x00), TAG, "dds 51 commit");

    return ESP_OK;
}

static esp_err_t lt8912b_write_rxlogicres(void)
{
    i2c_master_dev_handle_t m = s_lt.dev_main;

    ESP_RETURN_ON_ERROR(lt_write(m, 0x03, 0x7F), TAG, "rxres hold");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(lt_write(m, 0x03, 0xFF), TAG, "rxres release");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x05, 0xFB), TAG, "dds_rst hold");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(lt_write(m, 0x05, 0xFF), TAG, "dds_rst release");

    return ESP_OK;
}

static esp_err_t lt8912b_write_lvds_config(bool hdmi_mode)
{
    i2c_master_dev_handle_t m = s_lt.dev_main;

    ESP_RETURN_ON_ERROR(lt_write(m, 0x44, 0x30), TAG, "lvds 44");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x51, 0x05), TAG, "lvds 51");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x50, 0x24), TAG, "lvds 50");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x51, 0x2D), TAG, "lvds 51b");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x52, 0x04), TAG, "lvds 52");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x69, 0x0E), TAG, "lvds 69a");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x69, 0x8E), TAG, "lvds 69b");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x6A, 0x00), TAG, "lvds 6A");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x6C, 0xB8), TAG, "lvds 6C");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x6B, 0x51), TAG, "lvds 6B");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x04, 0xFB), TAG, "pll_rst hold");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x04, 0xFF), TAG, "pll_rst release");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x7F, 0x00), TAG, "lvds 7F");
    ESP_RETURN_ON_ERROR(lt_write(m, 0xA8, 0x13), TAG, "lvds A8");
    ESP_RETURN_ON_ERROR(lt_write(m, 0xB2, hdmi_mode ? 0x01 : 0x00), TAG, "lvds B2 mode");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x02, 0xF7), TAG, "lvds_pll hold");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x02, 0xFF), TAG, "lvds_pll release");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x03, 0xCF), TAG, "mipi_rx hold");
    ESP_RETURN_ON_ERROR(lt_write(m, 0x03, 0xFF), TAG, "mipi_rx release");

    return ESP_OK;
}

static void lt8912b_hpd_gpio_init(int gpio)
{
    if (gpio < 0) return;
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << gpio),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

static esp_err_t lt8912b_init_common(int hpd_gpio)
{
    esp_err_t ret = ESP_OK;

    uint8_t id_h = 0, id_l = 0;
    esp_err_t id_ret = ESP_ERR_TIMEOUT;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(10));
        if (lt_read(s_lt.dev_main, LT8912B_REG_CHIP_ID_H, &id_h) == ESP_OK &&
            lt_read(s_lt.dev_main, LT8912B_REG_CHIP_ID_L, &id_l) == ESP_OK) {
            if (id_h == LT8912B_CHIP_ID_H && id_l == LT8912B_CHIP_ID_L) {
                id_ret = ESP_OK;
                break;
            }
        }
    }
    if (id_ret != ESP_OK) {
        ESP_LOGE(TAG, "Chip ID mismatch: got 0x%02X%02X, expected 0x12B2", id_h, id_l);
        ret = ESP_ERR_NOT_FOUND;
        goto err_devs;
    }
    ESP_LOGI(TAG, "LT8912B detected (ID 0x%02X%02X)", id_h, id_l);

    ESP_GOTO_ON_ERROR(lt8912b_write_init_config(),  err_devs, TAG, "init_config");
    ESP_GOTO_ON_ERROR(lt8912b_write_mipi_basic(),   err_devs, TAG, "mipi_basic");
    ESP_GOTO_ON_ERROR(lt8912b_write_dds_config(),   err_devs, TAG, "dds_config");
    ESP_GOTO_ON_ERROR(lt8912b_write_video_timing(), err_devs, TAG, "video_timing");
    ESP_GOTO_ON_ERROR(lt8912b_write_rxlogicres(),   err_devs, TAG, "rxlogicres");
    ESP_GOTO_ON_ERROR(lt8912b_write_lvds_config(true), err_devs, TAG, "lvds_config");
    ESP_GOTO_ON_ERROR(lt_write(s_lt.dev_main, 0x33, 0x0E), err_devs, TAG, "hdmi_out_en");

    s_lt.hpd_gpio = hpd_gpio;
    lt8912b_hpd_gpio_init(s_lt.hpd_gpio);

    s_lt.initialized = true;
    ESP_LOGI(TAG, "LT8912B initialized — %dx%d@%dMHz HDMI output",
             CONFIG_SIM_DISPLAY_HACT, CONFIG_SIM_DISPLAY_VACT, CONFIG_SIM_DISPLAY_PCLK_MHZ);

    if (esp_lcd_lt8912b_is_connected()) {
        ESP_LOGI(TAG, "HDMI cable connected");
    } else {
        ESP_LOGW(TAG, "HDMI cable not detected");
    }
    return ESP_OK;

err_devs:
    if (s_lt.dev_audio)   { i2c_master_bus_rm_device(s_lt.dev_audio);   s_lt.dev_audio   = NULL; }
    if (s_lt.dev_cec_dsi) { i2c_master_bus_rm_device(s_lt.dev_cec_dsi); s_lt.dev_cec_dsi = NULL; }
    if (s_lt.dev_main)    { i2c_master_bus_rm_device(s_lt.dev_main);     s_lt.dev_main    = NULL; }
    if (s_lt.bus_owned && s_lt.bus) { i2c_del_master_bus(s_lt.bus); s_lt.bus = NULL; }
    memset(&s_lt, 0, sizeof(s_lt));
    return ret;
}

esp_err_t esp_lcd_lt8912b_init(const esp_lcd_lt8912b_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_FALSE(!s_lt.initialized, ESP_ERR_INVALID_STATE, TAG, "already initialized");

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port            = config->i2c_port,
        .sda_io_num          = config->sda_gpio,
        .scl_io_num          = config->scl_gpio,
        .clk_source          = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt   = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_lt.bus), TAG, "i2c_new_master_bus");
    s_lt.bus_owned = true;

    i2c_device_config_t dev_cfg = { .scl_speed_hz = 400000,
                                    .device_address = LT8912B_ADDR_MAIN };
    if (i2c_master_bus_add_device(s_lt.bus, &dev_cfg, &s_lt.dev_main) != ESP_OK) {
        i2c_del_master_bus(s_lt.bus);
        memset(&s_lt, 0, sizeof(s_lt));
        return ESP_FAIL;
    }
    dev_cfg.device_address = LT8912B_ADDR_CEC_DSI;
    if (i2c_master_bus_add_device(s_lt.bus, &dev_cfg, &s_lt.dev_cec_dsi) != ESP_OK) {
        i2c_master_bus_rm_device(s_lt.dev_main);
        i2c_del_master_bus(s_lt.bus);
        memset(&s_lt, 0, sizeof(s_lt));
        return ESP_FAIL;
    }
    dev_cfg.device_address = LT8912B_ADDR_AUDIO;
    if (i2c_master_bus_add_device(s_lt.bus, &dev_cfg, &s_lt.dev_audio) != ESP_OK) {
        i2c_master_bus_rm_device(s_lt.dev_cec_dsi);
        i2c_master_bus_rm_device(s_lt.dev_main);
        i2c_del_master_bus(s_lt.bus);
        memset(&s_lt, 0, sizeof(s_lt));
        return ESP_FAIL;
    }

    return lt8912b_init_common(config->hpd_gpio);
}

esp_err_t esp_lcd_lt8912b_init_with_bus(i2c_master_bus_handle_t bus,
                                         const esp_lcd_lt8912b_config_t *config)
{
    ESP_RETURN_ON_FALSE(bus    != NULL, ESP_ERR_INVALID_ARG,   TAG, "bus is NULL");
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG,   TAG, "config is NULL");
    ESP_RETURN_ON_FALSE(!s_lt.initialized, ESP_ERR_INVALID_STATE, TAG, "already initialized");

    s_lt.bus       = bus;
    s_lt.bus_owned = false;

    i2c_device_config_t dev_cfg = { .scl_speed_hz = 400000,
                                    .device_address = LT8912B_ADDR_MAIN };
    if (i2c_master_bus_add_device(s_lt.bus, &dev_cfg, &s_lt.dev_main) != ESP_OK) {
        memset(&s_lt, 0, sizeof(s_lt));
        return ESP_FAIL;
    }
    dev_cfg.device_address = LT8912B_ADDR_CEC_DSI;
    if (i2c_master_bus_add_device(s_lt.bus, &dev_cfg, &s_lt.dev_cec_dsi) != ESP_OK) {
        i2c_master_bus_rm_device(s_lt.dev_main);
        memset(&s_lt, 0, sizeof(s_lt));
        return ESP_FAIL;
    }
    dev_cfg.device_address = LT8912B_ADDR_AUDIO;
    if (i2c_master_bus_add_device(s_lt.bus, &dev_cfg, &s_lt.dev_audio) != ESP_OK) {
        i2c_master_bus_rm_device(s_lt.dev_cec_dsi);
        i2c_master_bus_rm_device(s_lt.dev_main);
        memset(&s_lt, 0, sizeof(s_lt));
        return ESP_FAIL;
    }

    return lt8912b_init_common(config->hpd_gpio);
}

bool esp_lcd_lt8912b_is_connected(void)
{
    if (!s_lt.initialized) return false;
    if (s_lt.hpd_gpio >= 0) {
        if (!gpio_get_level(s_lt.hpd_gpio)) return false;
    }
    uint8_t val = 0;
    if (lt_read(s_lt.dev_main, LT8912B_REG_HPD_STATUS, &val) != ESP_OK) return false;
    return (val & 0x80) != 0;
}

i2c_master_bus_handle_t esp_lcd_lt8912b_get_i2c_bus(void)
{
    return s_lt.initialized ? s_lt.bus : NULL;
}

esp_err_t esp_lcd_lt8912b_post_dpi_enable(void)
{
    if (!s_lt.initialized) return ESP_ERR_INVALID_STATE;

    i2c_master_dev_handle_t m = s_lt.dev_main;

    uint8_t hs_l = 0, hs_h = 0, vs_l = 0, vs_h = 0;
    lt_read(m, 0x9C, &hs_l);
    lt_read(m, 0x9D, &hs_h);
    lt_read(m, 0x9E, &vs_l);
    lt_read(m, 0x9F, &vs_h);
    /* Do NOT re-trigger rxlogicres/dds_rst here — the chip already locked
     * during init (DSI starts before post_dpi_enable is called). A second
     * rxlogicres pulse resets the PLL and breaks sync (Vsync drops from
     * 0x01D8 to 0x003D). Diagnostic read only. */
    ESP_LOGI(TAG, "MIPI sync: Hsync=0x%02X%02X Vsync=0x%02X%02X%s",
             hs_h, hs_l, vs_h, vs_l,
             (hs_l || hs_h || vs_l || vs_h) ? " — DSI locked" : " — NO DSI SIGNAL");

    return ESP_OK;
}

void esp_lcd_lt8912b_deinit(void)
{
    if (!s_lt.initialized) return;

    if (s_lt.dev_audio)   i2c_master_bus_rm_device(s_lt.dev_audio);
    if (s_lt.dev_cec_dsi) i2c_master_bus_rm_device(s_lt.dev_cec_dsi);
    if (s_lt.dev_main)    i2c_master_bus_rm_device(s_lt.dev_main);
    if (s_lt.bus_owned && s_lt.bus) i2c_del_master_bus(s_lt.bus);

    memset(&s_lt, 0, sizeof(s_lt));
    ESP_LOGI(TAG, "LT8912B de-initialized");
}
