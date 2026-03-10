#pragma once
#include "esp_err.h"
#include "driver/i2c_master.h"
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the HDMI display via LT8912B bridge.
 *
 * Sequence:
 *   1. LDO3 @ 2500mV for MIPI DPHY
 *   2. DSI bus (2 lanes, CONFIG_SIM_DISPLAY_DSI_LANE_MBPS)
 *   3. EoTP patch (LT8912B cannot handle EoTP)
 *   4. Shared I2C bus (SDA=CONFIG_SIM_DISPLAY_SDA_GPIO, SCL=CONFIG_SIM_DISPLAY_SCL_GPIO)
 *   5. LT8912B init with shared bus
 *   6. DPI panel (RGB888, 2 framebuffers, CONFIG_SIM_DISPLAY_HACT x CONFIG_SIM_DISPLAY_VACT)
 *   7. Clear framebuffers + esp_cache_msync
 *   8. esp_lcd_panel_init() + esp_lcd_panel_disp_on_off(true)
 *   9. esp_lcd_lt8912b_post_dpi_enable()
 *
 * @return ESP_OK on success, or an esp_err_t on failure.
 */
esp_err_t sim_display_init(void);

/**
 * @brief Get the shared I2C bus handle (for sharing with sim_audio).
 *
 * @return I2C master bus handle, or NULL if not initialized.
 */
i2c_master_bus_handle_t sim_display_get_i2c_bus(void);

/**
 * @brief Get both framebuffer pointers.
 *
 * @param fb0  Pointer to receive framebuffer 0 address
 * @param fb1  Pointer to receive framebuffer 1 address
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not initialized.
 */
esp_err_t sim_display_get_framebuffer(void **fb0, void **fb1);

/**
 * @brief Get the current back buffer (the one to render into).
 *
 * @return Pointer to back buffer, or NULL if not initialized.
 */
void *sim_display_get_back_buffer(void);

/**
 * @brief Flush the back buffer to display and swap front/back.
 *
 * Syncs the back buffer cache to PSRAM (C2M), then swaps the buffer indices
 * so the next call to sim_display_get_back_buffer() returns the other buffer.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not initialized.
 */
esp_err_t sim_display_flush(void);

/**
 * @brief Flush only a region of the back buffer, then swap front/back.
 *
 * Like sim_display_flush() but only syncs [byte_offset, byte_offset+byte_size)
 * of the back buffer.  Use this to avoid syncing unchanged padding rows.
 *
 * @param byte_offset  Byte offset from start of back buffer to start syncing.
 * @param byte_size    Number of bytes to sync.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not initialized.
 */
esp_err_t sim_display_flush_region(size_t byte_offset, size_t byte_size);

#ifdef __cplusplus
}
#endif
