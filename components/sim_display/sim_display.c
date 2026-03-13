/*
 * sim_display.c — HDMI display driver for SimCoupe (Olimex ESP32-P4-PC)
 *
 * Uses the production LT8912B driver architecture:
 *   - Three I2C panel IOs (main=0x48, cec_dsi=0x49, avi=0x4A)
 *   - esp_lcd_new_panel_lt8912b() — standard ESP-LCD panel interface
 *   - esp_lcd_panel_reset() + esp_lcd_panel_init() — full production init sequence
 *
 * Hardware:
 *   MIPI DSI: 2-lane, CONFIG_SIM_DISPLAY_DSI_LANE_MBPS Mbps
 *   LT8912B:  I2C1 SDA=GPIO7 SCL=GPIO8 (shared with ES8311 audio codec)
 *   HPD:      GPIO15
 *   Output:   CONFIG_SIM_DISPLAY_HACT × CONFIG_SIM_DISPLAY_VACT @ CONFIG_SIM_DISPLAY_PCLK_MHZ MHz
 *   Format:   RGB888, double-buffered (2 framebuffers in PSRAM)
 */

#include "sim_display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_ldo_regulator.h"
#include "esp_cache.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_lt8912b.h"
#include <string.h>

static const char *TAG = "sim_display";
static esp_lcd_panel_handle_t s_panel    = NULL;
static i2c_master_bus_handle_t s_i2c_bus = NULL;
static void *s_fb[2]  = {NULL, NULL};
static int   s_back_buf = 1;  /* index of back buffer (the one we write to) */

/* Switch the DPI panel's active (front) framebuffer by writing cur_fb_index
 * directly into the panel struct.
 *
 * Layout of esp_lcd_dpi_panel_t (esp_lcd_panel_dpi.c, IDF 5.5.x, RV32):
 *   struct esp_lcd_panel_t base:
 *     10 fn-ptrs (reset/init/del/draw_bitmap/mirror/swap_xy/set_gap/
 *                 invert_color/disp_on_off/disp_sleep) + void* user_data
 *     = 11 × 4 = 44 bytes
 *   esp_lcd_dsi_bus_handle_t bus:  offset 44, 4 bytes
 *   uint8_t virtual_channel:       offset 48
 *   uint8_t cur_fb_index:          offset 49  ← we write this
 *
 * Verified by disassembling dpi_panel_init: "sb zero, 49(s0)" sets cur_fb_index=0.
 * A uint8_t store is atomic on RISC-V — no mutex needed.
 * The DW-GDMA ISR reads cur_fb_index at frame-start; we write it any time. */
static inline void dpi_set_cur_fb(esp_lcd_panel_handle_t panel, uint8_t idx)
{
    uintptr_t base = (uintptr_t)panel;
    /* 11 ptrs (base) + 1 ptr (bus) + 1 byte (virtual_channel) = offset 49 */
    size_t off = 11 * sizeof(void *) + sizeof(void *) + 1;  /* = 49 on RV32 */
    volatile uint8_t *p = (volatile uint8_t *)(base + off);
    *p = idx;
}

esp_err_t sim_display_init(void)
{
    if (s_panel) return ESP_ERR_INVALID_STATE;

    /* Step 0: Power on MIPI DSI PHY via internal LDO regulator (LDO_VO3 → 2500 mV). */
    {
        esp_ldo_channel_handle_t ldo_dphy = NULL;
        esp_ldo_channel_config_t ldo_cfg = {
            .chan_id    = 3,
            .voltage_mv = 2500,
        };
        esp_err_t ldo_ret = esp_ldo_acquire_channel(&ldo_cfg, &ldo_dphy);
        if (ldo_ret != ESP_OK) {
            ESP_LOGW(TAG, "LDO3 acquire failed (0x%x) — PHY power may come from external supply",
                     ldo_ret);
        } else {
            ESP_LOGI(TAG, "MIPI DPHY powered via LDO3 @ 2500mV");
        }
    }

    /* Step 1: Create MIPI DSI bus */
    ESP_LOGI(TAG, "step 1 — DSI bus init");
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id             = 0,
        .num_data_lanes     = CONFIG_SIM_DISPLAY_DSI_LANES,
        .phy_clk_src        = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = CONFIG_SIM_DISPLAY_DSI_LANE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus), TAG, "DSI bus init failed");
    ESP_LOGI(TAG, "step 1 OK");

    /* Step 2: Create shared I2C bus */
    ESP_LOGI(TAG, "step 2 — shared I2C bus");
    {
        i2c_master_bus_config_t bus_cfg_i2c = {
            .i2c_port            = CONFIG_SIM_DISPLAY_I2C_PORT,
            .sda_io_num          = CONFIG_SIM_DISPLAY_SDA_GPIO,
            .scl_io_num          = CONFIG_SIM_DISPLAY_SCL_GPIO,
            .clk_source          = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt   = 7,
            .flags.enable_internal_pullup = true,
        };
        if (i2c_new_master_bus(&bus_cfg_i2c, &s_i2c_bus) != ESP_OK) {
            ESP_LOGE(TAG, "shared I2C bus creation failed");
            esp_lcd_del_dsi_bus(dsi_bus);
            return ESP_FAIL;
        }

        /* Scan the bus to verify devices are present (diagnostic only) */
        ESP_LOGI(TAG, "I2C%d scan (SDA=%d SCL=%d):",
                 CONFIG_SIM_DISPLAY_I2C_PORT,
                 CONFIG_SIM_DISPLAY_SDA_GPIO, CONFIG_SIM_DISPLAY_SCL_GPIO);
        for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
            if (i2c_master_probe(s_i2c_bus, addr, 20) == ESP_OK) {
                ESP_LOGI(TAG, "  I2C device found at 0x%02X", addr);
            }
        }
    }
    ESP_LOGI(TAG, "step 2 OK");

    /* Step 3: Create three I2C panel IOs for LT8912B (main, cec_dsi, avi) */
    ESP_LOGI(TAG, "step 3 — LT8912B I2C panel IOs");
    esp_lcd_panel_io_handle_t io_main    = NULL;
    esp_lcd_panel_io_handle_t io_cec_dsi = NULL;
    esp_lcd_panel_io_handle_t io_avi     = NULL;

    esp_lcd_panel_io_i2c_config_t io_cfg_main = LT8912B_IO_CFG(400 * 1000, LT8912B_IO_I2C_MAIN_ADDRESS);
    esp_lcd_panel_io_i2c_config_t io_cfg_cec  = LT8912B_IO_CFG(400 * 1000, LT8912B_IO_I2C_CEC_ADDRESS);
    esp_lcd_panel_io_i2c_config_t io_cfg_avi  = LT8912B_IO_CFG(400 * 1000, LT8912B_IO_I2C_AVI_ADDRESS);

    if (esp_lcd_new_panel_io_i2c(s_i2c_bus, &io_cfg_main, &io_main) != ESP_OK ||
        esp_lcd_new_panel_io_i2c(s_i2c_bus, &io_cfg_cec,  &io_cec_dsi) != ESP_OK ||
        esp_lcd_new_panel_io_i2c(s_i2c_bus, &io_cfg_avi,  &io_avi) != ESP_OK) {
        ESP_LOGE(TAG, "LT8912B panel IO creation failed");
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
        esp_lcd_del_dsi_bus(dsi_bus);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "step 3 OK");

    /* Step 4: DPI panel config */
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .dpi_clk_src         = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz  = CONFIG_SIM_DISPLAY_PCLK_MHZ,
        .virtual_channel     = 0,
        .in_color_format     = LCD_COLOR_PIXEL_FORMAT_RGB888,
        .num_fbs             = 2,
        .video_timing = {
            .h_size            = CONFIG_SIM_DISPLAY_HACT,
            .v_size            = CONFIG_SIM_DISPLAY_VACT,
            .hsync_pulse_width = CONFIG_SIM_DISPLAY_HS,
            .hsync_back_porch  = CONFIG_SIM_DISPLAY_HBP,
            .hsync_front_porch = CONFIG_SIM_DISPLAY_HFP,
            .vsync_pulse_width = CONFIG_SIM_DISPLAY_VS,
            .vsync_back_porch  = CONFIG_SIM_DISPLAY_VBP,
            .vsync_front_porch = CONFIG_SIM_DISPLAY_VFP,
        },
        .flags.disable_lp    = true,
    };

    /* Step 5: Video timing for LT8912B */
    esp_lcd_panel_lt8912b_video_timing_t video_timing = ESP_LCD_LT8912B_VIDEO_TIMING_1920x1080_30Hz();

    /* Step 6: Vendor config */
    lt8912b_vendor_config_t vendor_cfg = {
        .video_timing = video_timing,
        .mipi_config = {
            .dsi_bus    = dsi_bus,
            .dpi_config = &dpi_cfg,
            .lane_num   = CONFIG_SIM_DISPLAY_DSI_LANES,
        },
    };

    /* Step 7: Panel device config */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = CONFIG_SIM_DISPLAY_HPD_GPIO >= 0 ? -1 : -1,  /* no reset GPIO */
        .vendor_config  = &vendor_cfg,
    };

    /* Step 8: Create LT8912B panel (wraps DPI panel) */
    ESP_LOGI(TAG, "step 4-8 — esp_lcd_new_panel_lt8912b @ %dx%d pclk=%dMHz",
             CONFIG_SIM_DISPLAY_HACT, CONFIG_SIM_DISPLAY_VACT, CONFIG_SIM_DISPLAY_PCLK_MHZ);
    esp_lcd_panel_lt8912b_io_t io_all = {
        .main    = io_main,
        .cec_dsi = io_cec_dsi,
        .avi     = io_avi,
    };
    if (esp_lcd_new_panel_lt8912b(&io_all, &panel_cfg, &s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_lt8912b failed");
        esp_lcd_panel_io_del(io_avi);
        esp_lcd_panel_io_del(io_cec_dsi);
        esp_lcd_panel_io_del(io_main);
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
        esp_lcd_del_dsi_bus(dsi_bus);
        return ESP_FAIL;
    }

    /* Step 9: Reset + Init — this triggers the full LT8912B init sequence */
    ESP_LOGI(TAG, "step 9 — panel reset + init");
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    ESP_LOGI(TAG, "step 9 OK");

    /* Step 10: Get framebuffer pointers, clear to black, flush cache → PSRAM */
    ESP_LOGI(TAG, "step 10 — clear framebuffers");
    if (esp_lcd_dpi_panel_get_frame_buffer(s_panel, 2, &s_fb[0], &s_fb[1]) == ESP_OK) {
        size_t fb_size = CONFIG_SIM_DISPLAY_HACT * CONFIG_SIM_DISPLAY_VACT * 3;
        for (int i = 0; i < 2; i++) {
            if (s_fb[i]) {
                memset(s_fb[i], 0, fb_size);
                esp_cache_msync(s_fb[i], fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            }
        }
    }
    ESP_LOGI(TAG, "step 10 OK");

    /* Step 11: Wait for DPI signal to stabilise, then re-trigger LT8912B input detection.
     *
     * panel_lt8912b_init() calls detect_input BEFORE the DPI is active (it sees 0xff/0xff).
     * After lt8912b->init(panel) starts the DPI, the LT8912B needs to re-lock to the signal.
     * esp_lcd_panel_lt8912b_is_ready() calls detect_input + checks HPD — calling it repeatedly
     * gives the chip time to lock and re-programs the video setup with the live signal. */
    ESP_LOGI(TAG, "step 11 — waiting for LT8912B to lock to DPI signal...");
    bool connected = false;
    for (int i = 0; i < 20; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        connected = esp_lcd_panel_lt8912b_is_ready(s_panel);
        if (connected) {
            ESP_LOGI(TAG, "  locked after %d ms", (i + 1) * 100);
            break;
        }
    }
    ESP_LOGI(TAG, "step 11 OK");

    ESP_LOGI(TAG, "%dx%d@%dMHz ready%s",
             CONFIG_SIM_DISPLAY_HACT, CONFIG_SIM_DISPLAY_VACT, CONFIG_SIM_DISPLAY_PCLK_MHZ,
             connected ? " (cable connected)" : " (no cable)");

    return ESP_OK;
}

i2c_master_bus_handle_t sim_display_get_i2c_bus(void)
{
    return s_i2c_bus;
}

esp_err_t sim_display_get_framebuffer(void **fb0, void **fb1)
{
    if (!s_panel) return ESP_ERR_INVALID_STATE;
    *fb0 = s_fb[0];
    *fb1 = s_fb[1];
    return ESP_OK;
}

void *sim_display_get_back_buffer(void)
{
    if (!s_panel) return NULL;
    return s_fb[s_back_buf];
}

esp_err_t sim_display_flush(void)
{
    if (!s_panel) return ESP_ERR_INVALID_STATE;
    size_t fb_size = CONFIG_SIM_DISPLAY_HACT * CONFIG_SIM_DISPLAY_VACT * 3;
    esp_cache_msync(s_fb[s_back_buf], fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    dpi_set_cur_fb(s_panel, (uint8_t)s_back_buf);
    s_back_buf ^= 1;
    return ESP_OK;
}

esp_err_t sim_display_flush_region(size_t byte_offset, size_t byte_size)
{
    if (!s_panel) return ESP_ERR_INVALID_STATE;
    uint8_t *base = (uint8_t *)s_fb[s_back_buf];
    esp_cache_msync(base + byte_offset, byte_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    return ESP_OK;
}

esp_err_t sim_display_swap(void)
{
    if (!s_panel) return ESP_ERR_INVALID_STATE;
    dpi_set_cur_fb(s_panel, (uint8_t)s_back_buf);
    s_back_buf ^= 1;
    return ESP_OK;
}
