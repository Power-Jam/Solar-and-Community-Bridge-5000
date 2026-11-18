#pragma once

#include <stdint.h>

#define LCD_GPIO_RST 41
#define LCD_GPIO_POWER 29
#define LCD_GPIO_BLIGHT 31
#define BLIGHT_PWM_HZ 30000
#define BLIGHT_ON_LEVEL 1
// Backlight brightness in percent: normal use, and after BLIGHT_DIM_AFTER_MS without a touch.
#define BLIGHT_ACTIVE_PERCENT 40
#define BLIGHT_DIM_PERCENT 10
#define BLIGHT_DIM_AFTER_MS 60000

#define TOUCH_GPIO_RST 40
#define TOUCH_GPIO_INT 42
#define I2C_GPIO_SCL 46
#define I2C_GPIO_SDA 45

#define RFID1_GPIO_SS 10
#define RFID2_GPIO_SS 9
#define RFID3_GPIO_SS 28
#define RFID_GPIO_SCK 6
#define RFID_GPIO_MOSI 7
#define RFID_GPIO_MISO 8

#define PANEL_WIDTH 1024
#define PANEL_HEIGHT 600
#define LCD_CLK_MHZ 51
#define LCD_HPW 70
#define LCD_HBP 160
#define LCD_HFP 160
#define LCD_VPW 10
#define LCD_VBP 23
#define LCD_VFP 21

// LoRaWAN OTAA credentials; replace these placeholders with the TTN device values.
#define LORA_DEV_EUI UINT64_C(0x70B3D57ED0079BC7)
#define LORA_JOIN_EUI UINT64_C(0x0000000000000000)
#define LORA_APP_KEY {0x5A, 0xE4, 0x45, 0xD1, 0x84, 0xE3, 0x2A, 0x0C, 0xEB, 0x27, 0xBB, 0x69, 0x83, 0x05, 0xC0, 0xF9}
#define LORA_NWK_KEY {0x73, 0xBF, 0xAC, 0x43, 0xE1, 0x52, 0xBF, 0xA0, 0x0E, 0x1B, 0x64, 0x35, 0xBF, 0xC9, 0x76, 0x87}

#define LORA_GPIO_SCK 2
#define LORA_GPIO_MOSI 3
#define LORA_GPIO_MISO 4
#define LORA_GPIO_CS 5
#define LORA_GPIO_RESET 25
#define LORA_GPIO_BUSY 49
#define LORA_GPIO_IRQ 50
#define LORA_GPIO_RX_ENABLE 51
#define LORA_GPIO_TX_ENABLE 52

#define LORA_TCXO_VOLTAGE 1.8f
#define LORA_AIRTIME_ALLOWANCE_MS_PER_HOUR 1250

#define INA_ADDRESS 0x40
#define INA_SHUNT_OHMS 0.1f
#define INA_MAX_CURRENT_A 0.8f

#define GPS_GPIO_RX 48
#define GPS_BAUD 9600

#define TEMP_GPIO_DATA 53
// 1 = send a mock temperature over LoRa when the DS18B20 has no fresh reading (sensor broken/missing).
#define TEMP_MOCK_WHEN_MISSING 1

#define AUDIO_FILE "/audio/demo.wav"
#define AUDIO_GPIO_BCLK 22
#define AUDIO_GPIO_LRCLK 21
#define AUDIO_GPIO_DOUT 23
#define AUDIO_GPIO_AMP 30
#define AUDIO_AMP_ON_LEVEL LOW
