#pragma once

// GPIO pin assignments
#define PICO_IN_PIN      17  // CMT input
#define PICO_OUT_PIN     19  // CMT output
#define REMOTE_PIN       18  // Remote detect input
#define BUZZER_PIN       16

#define PICO_I2C_SDA_PIN 10 // OLED I2C SDA
#define PICO_I2C_SCL_PIN 11 // OLED I2C SCL

#define PICO_SD_CS_PIN   13  // SD card SPI CS
#define PICO_SPI_MISO_PIN 12 // SD card SPI MISO(RX)
#define PICO_SPI_MOSI_PIN 15 // SD card SPI MOSI(TX)
#define PICO_SPI_CLK_PIN  14 // SD card SPI SCK
#define PICO_SPI_DETECT_PIN 9 // SD card detect input

#define BTN_REC_PIN      5
#define BTN_PLAY_PIN     4
#define BTN_STOP_PIN     6
#define BTN_OK_PIN       7
#define BTN_CANCEL_PIN   8
#define BTN_UP_PIN       1
#define BTN_DOWN_PIN     3
#define BTN_LEFT_PIN     0
#define BTN_RIGHT_PIN    2
#define NUM_BUTTONS      9

