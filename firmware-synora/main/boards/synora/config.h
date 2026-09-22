#ifndef SYNORA_BOARD_CONFIG_H_
#define SYNORA_BOARD_CONFIG_H_

#include <driver/gpio.h>
#include <hal/adc_types.h>
#include <hal/spi_types.h>
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

// DHT11 (air temp/humidity), single-wire bit-banged (see dht11_sensor.h) --
// replaces the SHT31 (was I2C_NUM_0, GPIO8/9). Reuses GPIO9 (was SHT31's
// SDA); GPIO8 (was SCL) is now free. BH1750 (ambient light) dropped for v1,
// see project_synora_board memory.
#define DHT11_GPIO GPIO_NUM_9

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

// ILI9341 SPI TFT, 240x320 -- replaces the ST7793 8-bit-parallel (I80) 240x400
// panel above. Driven in the controller's native portrait orientation (its
// internal RAM is 240 wide x 320 tall), so no swap_xy/mirror is needed the
// way a landscape mount would require. Unlike the ST7793, ILI9341 has a real
// esp_lcd vendor driver (espressif/esp_lcd_ili9341, see main/idf_component.yml
// and esp_lcd_new_panel_ili9341() in synora_display.cc) -- no hand-rolled
// register-init table needed here.
//
// 4-wire SPI (CS/DC/RST/SCK/MOSI; the panel is write-only, no MISO) needs far
// fewer GPIOs than the old 8-bit parallel bus, so this reuses the first 5 pins
// of the old CS/DC/RST/WR/D0-D7 ribbon (GPIO10-14) and frees the rest
// (GPIO16-18, 21, 38, 42) for future use. If your module's backlight (LED/
// BLK) pin needs active GPIO control rather than being hardwired to 3V3,
// wire it to one of those freed pins (e.g. GPIO_NUM_15) and drive it high
// after Init() -- not wired up by default since many ILI9341 breakouts tie
// it straight to 3V3.
#define DISPLAY_WIDTH     240
#define DISPLAY_HEIGHT    320
#define DISPLAY_SPI_HOST  SPI2_HOST
#define DISPLAY_CS_GPIO   GPIO_NUM_10
#define DISPLAY_DC_GPIO   GPIO_NUM_11
#define DISPLAY_RST_GPIO  GPIO_NUM_12
#define DISPLAY_SCK_GPIO  GPIO_NUM_13
#define DISPLAY_MOSI_GPIO GPIO_NUM_14

// Water pump (IRLZ44N logic-level MOSFET gate -- active-high drive).
#define PUMP_GPIO GPIO_NUM_47

// How often the telemetry reporter reads all sensors and POSTs to the server.
#define TELEMETRY_REPORT_INTERVAL_SEC 60

// How long WiFi must stay disconnected (while paired) before the device
// resumes BLE advertising so the phone app can reconnect it or hand it new
// credentials -- see SynoraBoard::CheckWifiLossRecovery(). Well past the
// WiFi driver's own reconnect/backoff window (a few seconds), so this only
// fires on genuinely sustained loss, not a brief blip.
#define WIFI_LOST_BLE_FALLBACK_SEC 90

// GPIO19/20 are the native-USB D-/D+ pins, 22-25 don't exist on the chip,
// 26-32 are reserved by SPI flash, 33-37 by octal PSRAM
// (CONFIG_SPIRAM_MODE_OCT), 3/45/46 are strapping pins, 43/44 are the
// console UART -- none of the above (or BUILTIN_LED_GPIO/PUMP_GPIO/
// WATER_LEVEL_*_GPIO above) use them.

#endif // SYNORA_BOARD_CONFIG_H_
