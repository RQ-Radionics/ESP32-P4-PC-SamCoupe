/*
 * esp_lcd_lt8912b_eotp.c — EoTP patch for the LT8912B HDMI bridge
 *
 * Separate compilation unit to avoid TAG macro conflict with mipi_dsi_priv.h.
 * mipi_dsi_priv.h defines #define TAG "lcd.dsi" which conflicts with the
 * static TAG in the main driver file.
 */

#include "mipi_dsi_priv.h"
#include "hal/mipi_dsi_host_ll.h"

#include "esp_err.h"
#include "esp_check.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_lt8912b.h"

#ifndef TAG
static const char *TAG = "lt8912b_eotp";
#endif

esp_err_t esp_lcd_lt8912b_patch_dsi_eotp(esp_lcd_dsi_bus_handle_t dsi_bus)
{
    ESP_RETURN_ON_FALSE(dsi_bus != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "lt8912b: dsi_bus is NULL for EoTP patch");

    esp_lcd_dsi_bus_t *bus = (esp_lcd_dsi_bus_t *)dsi_bus;
    mipi_dsi_host_ll_enable_tx_eotp(bus->hal.host, false, false);

    ESP_LOGI(TAG, "lt8912b: DSI EoTP disabled (LT8912B requires no EoTP)");
    return ESP_OK;
}
