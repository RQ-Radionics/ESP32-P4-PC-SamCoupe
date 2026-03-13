/*
 * esp_lcd_lt8912b.h — LT8912B MIPI DSI → HDMI bridge driver
 *
 * Lontium LT8912B: 2-lane MIPI DSI input, HDMI/DVI output.
 *
 * Hardware (Olimex ESP32-P4-PC Rev B):
 *   I2C1  SDA=GPIO7  SCL=GPIO8  (400 kHz, shared with ES8311)
 *   I2C addresses: 0x48 (main), 0x49 (MIPI/DDS), 0x4A (audio/AVI, unused)
 *   HPD input:  GPIO15  (HDMI hot-plug detect, active HIGH)
 *   DSI:        2 lanes, DATA0 + CLK, dedicated P4 DSI pads
 *
 * Reference:
 *   Linux kernel driver: drivers/gpu/drm/bridge/lontium-lt8912b.c
 */

#pragma once

#include "esp_err.h"
#include "esp_lcd_types.h"
#include "esp_lcd_mipi_dsi.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  i2c_port;      /**< I2C port number */
    int  sda_gpio;      /**< I2C SDA GPIO */
    int  scl_gpio;      /**< I2C SCL GPIO */
    int  hpd_gpio;      /**< HDMI HPD input GPIO, -1 to disable */
    bool hdmi_mode;     /**< true = HDMI output, false = DVI */
} esp_lcd_lt8912b_config_t;

esp_err_t esp_lcd_lt8912b_init(const esp_lcd_lt8912b_config_t *config);
esp_err_t esp_lcd_lt8912b_init_with_bus(i2c_master_bus_handle_t bus,
                                         const esp_lcd_lt8912b_config_t *config);
bool esp_lcd_lt8912b_is_connected(void);
esp_err_t esp_lcd_lt8912b_patch_dsi_eotp(esp_lcd_dsi_bus_handle_t dsi_bus);
i2c_master_bus_handle_t esp_lcd_lt8912b_get_i2c_bus(void);
esp_err_t esp_lcd_lt8912b_post_dpi_enable(void);
void esp_lcd_lt8912b_deinit(void);

#ifdef __cplusplus
}
#endif
