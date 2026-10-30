#pragma once

// Keep the custom board config aligned with the installed ESP32_Display_Panel version.
// This board uses a DSI LCD, an I2C touch controller, and a PWM LEDC backlight.
#define ESP_PANEL_DRIVERS_BUS_USE_MIPI_DSI (1)
#define ESP_PANEL_DRIVERS_BUS_USE_I2C (1)
#define ESP_PANEL_DRIVERS_LCD_USE_EK79007 (1)
#define ESP_PANEL_DRIVERS_TOUCH_USE_GT911 (1)
#define ESP_PANEL_DRIVERS_BACKLIGHT_USE_PWM_LEDC (1)

#define ESP_PANEL_DRIVERS_CONF_FILE_VERSION_MAJOR 1
#define ESP_PANEL_DRIVERS_CONF_FILE_VERSION_MINOR 2
#define ESP_PANEL_DRIVERS_CONF_FILE_VERSION_PATCH 0
