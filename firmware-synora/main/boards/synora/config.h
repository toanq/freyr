#ifndef SYNORA_BOARD_CONFIG_H_
#define SYNORA_BOARD_CONFIG_H_

#include <driver/gpio.h>
#include <hal/adc_types.h>
#include <driver/i2c_types.h>

// Both match the server's fixed voice audio_params (16kHz/60ms, see
// synora_voice_session.cc) so no resampler is needed on either leg.
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 16000

// Simplex I2S: separate mic (INMP441) and amp (MAX98357A) buses, no I2C audio codec.
#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_41
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_39
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_40

#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_5
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_6
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_7

#define BUILTIN_LED_GPIO GPIO_NUM_48

// BOOT button (GPIO0, strapping pin). Holding it triggers a factory reset;
// a click toggles the voice session (SynoraBoard::InitializeVoiceButton())
// -- the only physical button on this board, doing double duty.
#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define FACTORY_RESET_HOLD_MS 10000

// I2C bus for SHT31 (air temp/humidity) only -- NOT shared with the water-level
// sensor below. BH1750 (ambient light) dropped for v1, see project_synora_board
// memory.
#define BOARD_I2C_PORT     I2C_NUM_0
#define BOARD_I2C_SDA_GPIO GPIO_NUM_9
#define BOARD_I2C_SCL_GPIO GPIO_NUM_8

#define SHT31_I2C_ADDR    0x44

// Water-tank level: VL53L1X (not L0X -- downgraded then un-downgraded, see
// project_synora_board memory), via saleca/vl53l1x_uld_esp_wrapper. On its
// OWN I2C port (I2C_NUM_1, GPIO2/4) rather than sharing BOARD_I2C_PORT above:
// that wrapper component creates its own internal i2c_master_bus_handle_t
// from raw pins and cannot be handed a pre-existing shared bus handle.
// ESP32-S3 has two hardware I2C controllers, so this costs nothing.
#define WATER_LEVEL_I2C_PORT     I2C_NUM_1
#define WATER_LEVEL_SDA_GPIO     GPIO_NUM_2
#define WATER_LEVEL_SCL_GPIO     GPIO_NUM_4
#define WATER_LEVEL_I2C_ADDR     0x29

// Sensor mounted at the tank top facing down at the water surface -- shorter
// measured distance means more water. These distances depend on the physical
// mounting height/enclosure and are NOT measured yet: placeholders borrowed
// from a differently-shaped enclosure in a sibling project, calibrate against
// the real tank before trusting the percentage.
#define WATER_LEVEL_EMPTY_DISTANCE_MM 46
#define WATER_LEVEL_FULL_DISTANCE_MM  9

// Capacitive soil-moisture sensor (replaces the resistive LM393 module --
// resistive probes corrode/drift). Needs its own dry/wet calibration.
// GPIO1 = ADC1_CH0 on ESP32-S3.
#define SOIL_MOISTURE_ADC_UNIT    ADC_UNIT_1
#define SOIL_MOISTURE_ADC_CHANNEL ADC_CHANNEL_0
#define SOIL_MOISTURE_ADC_GPIO    GPIO_NUM_1

// ST7793 8-bit parallel (I80) TFT display, 240x400 (Open-Smart 3.5" module;
// replaces the ST7735S GMT144-10 128x128 SPI panel). ST7793 is real, distinct
// Sitronix silicon -- not ST7796 -- confirmed from Sitronix's own datasheet
// and the module's TFT_eSPI reference driver (whose own window registers
// show HEA=0xEF/VEA=0x18F, i.e. 240x400; the "240x480" on the product listing
// doesn't match either the chip's 432-gate-line max or any known-working
// config, so 240x400 is what synora_display.cc actually drives). One
// GPIO10-18 ribbon for CS/DC/RST/WR/D0-D4; D5-D7 land past two hard gaps in
// the ESP32-S3's own pinout (see below) rather than staying contiguous --
// there's no way to route around those and keep one clean ribbon.
//
// Gap 1, GPIO19/20: the ESP32-S3's fixed native-USB D-/D+ pins. An earlier
// revision put D5/D6 here, which fought the native USB PHY for the pins the
// moment the I80 bus initialized and made the board crash and drop off USB
// entirely.
// Gap 2, GPIO22-25: these GPIO numbers don't exist on the ESP32-S3 at all
// (valid range is 0-21 and 26-48) -- an earlier revision assumed they did
// (used for PUMP/LED below) and it went undetected because nothing actually
// referenced those macros in code yet; first real use (D6/D7 here) failed
// to compile ("GPIO_NUM_22 was not declared").
#define DISPLAY_WIDTH  240
#define DISPLAY_HEIGHT 400
#define DISPLAY_CS_GPIO  GPIO_NUM_10
#define DISPLAY_DC_GPIO  GPIO_NUM_11
#define DISPLAY_RST_GPIO GPIO_NUM_12
#define DISPLAY_WR_GPIO  GPIO_NUM_13  // I80 write strobe / PCLK
#define DISPLAY_D0_GPIO  GPIO_NUM_14
#define DISPLAY_D1_GPIO  GPIO_NUM_15
#define DISPLAY_D2_GPIO  GPIO_NUM_16
#define DISPLAY_D3_GPIO  GPIO_NUM_17
#define DISPLAY_D4_GPIO  GPIO_NUM_18
// GPIO_NUM_19, GPIO_NUM_20: native USB D-/D+ -- do not use for anything.
#define DISPLAY_D5_GPIO  GPIO_NUM_21
// GPIO_NUM_22..GPIO_NUM_25: don't exist on ESP32-S3.
// GPIO_NUM_26..GPIO_NUM_32: SPI flash. GPIO_NUM_33..GPIO_NUM_37: octal PSRAM.
#define DISPLAY_D6_GPIO  GPIO_NUM_38
#define DISPLAY_D7_GPIO  GPIO_NUM_42

// Water pump (IRLZ44N logic-level MOSFET gate -- active-high drive).
#define PUMP_GPIO GPIO_NUM_47

// How often the telemetry reporter reads all sensors and POSTs to the server.
#define TELEMETRY_REPORT_INTERVAL_SEC 60

// GPIO19/20 are the native-USB D-/D+ pins, 22-25 don't exist on the chip,
// 26-32 are reserved by SPI flash, 33-37 by octal PSRAM
// (CONFIG_SPIRAM_MODE_OCT), 3/45/46 are strapping pins, 43/44 are the
// console UART -- none of the above (or BUILTIN_LED_GPIO/PUMP_GPIO/
// WATER_LEVEL_*_GPIO above) use them.

#endif // SYNORA_BOARD_CONFIG_H_
