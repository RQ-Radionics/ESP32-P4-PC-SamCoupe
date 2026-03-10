/*
 * sim_display.c — HDMI display driver for SimCoupe (Olimex ESP32-P4-PC)
 *
 * Wraps the LT8912B MIPI DSI → HDMI bridge initialization sequence,
 * extracted from esp32-mos/main/main.c hdmi_init() and adapted as a
 * standalone ESP-IDF component.
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
#include "esp_ldo_regulator.h"
#include "esp_cache.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_lt8912b.h"
#include "clk_ctrl_os.h"
#include <string.h>

static const char *TAG = "sim_display";
static esp_lcd_panel_handle_t s_panel    = NULL;
static i2c_master_bus_handle_t s_i2c_bus = NULL;
static void *s_fb[2]  = {NULL, NULL};
static int   s_back_buf = 1;  /* index of back buffer (the one we write to) */

/* Switch the DPI panel's active (front) framebuffer by writing cur_fb_index
 * directly into the panel struct.
 *
 * WHY NOT esp_lcd_panel_draw_bitmap():
 *   draw_bitmap fires on_color_trans_done from task context, which reprograms
 *   the DW-GDMA channel while the ISR may be reading it → Load access fault in
 *   dw_gdma_channel_default_isr (MTVAL=0x188, dev=0x0).  This is the exact
 *   crash seen on first Video::Update() call.
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

    /* Step 0: Power on MIPI DSI PHY via internal LDO regulator (LDO_VO3 → 2500 mV).
     * The DSI PHY requires VDD_MIPI_DPHY = 2.5V to start up.  Without this
     * the PHY PLL never locks and esp_lcd_new_dsi_bus() hangs indefinitely.
     * The LDO handle is kept alive for the lifetime of the application. */
    {
        esp_ldo_channel_handle_t ldo_dphy = NULL;
        esp_ldo_channel_config_t ldo_cfg = {
            .chan_id    = 3,      /* LDO_VO3 → VDD_MIPI_DPHY on ESP32-P4 devkits */
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
    if (esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus) != ESP_OK) {
        ESP_LOGE(TAG, "DSI bus init failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "step 1 OK");

    /* Step 2: Patch — disable EoTP (LT8912B cannot handle it) */
    ESP_LOGI(TAG, "step 2 — EoTP patch");
    if (esp_lcd_lt8912b_patch_dsi_eotp(dsi_bus) != ESP_OK) {
        ESP_LOGE(TAG, "EoTP patch failed");
        esp_lcd_del_dsi_bus(dsi_bus);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "step 2 OK");

    /* Step 3: Create shared I2C bus (ES8311 audio codec and LT8912B both live
     * on this bus — GPIO7/SDA, GPIO8/SCL, 400 kHz, I2C port CONFIG_SIM_DISPLAY_I2C_PORT).
     * Creating it here, before any driver touches I2C, guarantees a clean FSM state.
     * Both esp_lcd_lt8912b_init_with_bus() and sim_audio_init_with_bus() add their
     * devices to this same handle without fighting over bus ownership. */
    ESP_LOGI(TAG, "step 3 — shared I2C bus + LT8912B init");
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

    /* Initialize LT8912B using the shared bus handle (does NOT own the bus) */
    esp_lcd_lt8912b_config_t lt_cfg = {
        .hpd_gpio  = CONFIG_SIM_DISPLAY_HPD_GPIO,
        .hdmi_mode = CONFIG_SIM_DISPLAY_HDMI_MODE,
        /* i2c_port / sda_gpio / scl_gpio ignored by init_with_bus */
    };
    if (esp_lcd_lt8912b_init_with_bus(s_i2c_bus, &lt_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "LT8912B I2C init failed");
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
        esp_lcd_del_dsi_bus(dsi_bus);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "step 3 OK");

    /* Step 4: Create DPI panel — feeds pixel data from ESP32-P4 to LT8912B DSI input.
     * Clock source = APLL, configured to 74.25 MHz for CEA-861 720p@60Hz.
     * APLL is the only source that can generate 74.25 MHz exactly from the
     * 40 MHz XTAL — PLL_F240M cannot divide to 74.25 MHz without error.
     * num_fbs=2: double-buffered so SimCoupe renders to back buffer
     *            while DPI DMA reads the front buffer.
     * disable_lp=1: stay in HS mode during blanking (required for video mode). */
    ESP_LOGI(TAG, "step 4a — APLL init @ %d MHz", CONFIG_SIM_DISPLAY_PCLK_MHZ);
    {
        periph_rtc_apll_acquire();
        uint32_t real_freq_hz = 0;
        /* 74.25 MHz = 74250 kHz — CEA-861 720p@60Hz exact pixel clock.
         * CONFIG_SIM_DISPLAY_PCLK_MHZ is an integer (74) and loses the .25 MHz
         * fraction, so we pass the exact value in Hz directly. */
        esp_err_t apll_ret = periph_rtc_apll_freq_set(74250000, &real_freq_hz);
        if (apll_ret != ESP_OK) {
            ESP_LOGE(TAG, "APLL freq set failed (0x%x)", apll_ret);
            esp_lcd_lt8912b_deinit();
            i2c_del_master_bus(s_i2c_bus);
            s_i2c_bus = NULL;
            esp_lcd_del_dsi_bus(dsi_bus);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "APLL: requested %d MHz, actual %.3f MHz",
                 CONFIG_SIM_DISPLAY_PCLK_MHZ, real_freq_hz / 1e6f);
    }

    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .dpi_clk_src         = MIPI_DSI_DPI_CLK_SRC_APLL,
        .dpi_clock_freq_mhz  = CONFIG_SIM_DISPLAY_PCLK_MHZ,
        .pixel_format        = LCD_COLOR_PIXEL_FORMAT_RGB888,
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
        .flags.disable_lp    = 1,
    };
    ESP_LOGI(TAG, "step 4 — DPI panel create");
    if (esp_lcd_new_panel_dpi(dsi_bus, &dpi_cfg, &s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "DPI panel init failed");
        esp_lcd_lt8912b_deinit();
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
        esp_lcd_del_dsi_bus(dsi_bus);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "step 4 OK");

    /* Step 5: Get framebuffer pointers, clear to black, enable panel.
     * The IDF allocates framebuffers with calloc (heap_caps_calloc) so they
     * start as zero in the cache, but the DMA reads physical PSRAM which may
     * still contain stale data.  Flush cache → PSRAM before panel enable. */
    ESP_LOGI(TAG, "step 5 — clear framebuffers + panel enable");
    if (esp_lcd_dpi_panel_get_frame_buffer(s_panel, 2, &s_fb[0], &s_fb[1]) == ESP_OK) {
        size_t fb_size = CONFIG_SIM_DISPLAY_HACT * CONFIG_SIM_DISPLAY_VACT * 3;  /* RGB888 */
        for (int i = 0; i < 2; i++) {
            if (s_fb[i]) {
                memset(s_fb[i], 0, fb_size);
                esp_cache_msync(s_fb[i], fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            }
        }
    }

    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_disp_on_off(s_panel, true);

    ESP_LOGI(TAG, "single framebuffer at %p", s_fb[0]);

    /* Read LT8912B diagnostic regs to confirm DSI is active */
    esp_lcd_lt8912b_post_dpi_enable();

    ESP_LOGI(TAG, "step 5 OK");
    ESP_LOGI(TAG, "%dx%d@%dMHz ready%s",
             CONFIG_SIM_DISPLAY_HACT, CONFIG_SIM_DISPLAY_VACT, CONFIG_SIM_DISPLAY_PCLK_MHZ,
             esp_lcd_lt8912b_is_connected() ? " (cable connected)" : " (no cable)");

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
    /* msync only — no swap. Call sim_display_swap() once after all regions. */
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
