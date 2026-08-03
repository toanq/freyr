# Synora board wiring

Reference for physically wiring the board. GPIO assignments here must match
`config.h` — if you change one, change the other.

## Display: Open-Smart 3.5" TFT, ST7793 controller, 240x400, 8-bit parallel (I80)

The module's silkscreen says "ST7793" -- that's real, distinct Sitronix
silicon, not ST7796 (confirmed against Sitronix's own ST7793 datasheet and
the module's TFT_eSPI reference driver). The product listing says "240x480"
but the chip's own register map (and the reference driver) only goes up to
240x400 -- that's what `synora_display.cc` actually drives.

| Module pin | Connect to | Notes |
|---|---|---|
| CS | GPIO10 | |
| RS (DC) | GPIO11 | also touch panel X- (same net; firmware doesn't read touch) |
| RST | GPIO12 | |
| WR | GPIO13 | also touch panel Y+ (same net) |
| RD | **3.3V**, not a GPIO | ESP32-S3's I80 peripheral is write-only; tie RD permanently high |
| DB0 (D0) | GPIO14 | |
| DB1 (D1) | GPIO15 | |
| DB2 (D2) | GPIO16 | |
| DB3 (D3) | GPIO17 | |
| DB4 (D4) | GPIO18 | |
| DB5 (D5) | GPIO21 | skips GPIO19/20, see gotchas below |
| DB6 (D6) | GPIO38 | skips GPIO22-37, see gotchas below |
| DB7 (D7) | GPIO42 | also touch panel Y- (same net) |
| VCC | 3.3V | logic power |
| GND | GND | |
| LED (backlight) | **GND** | active-low/cathode pin -- confirmed by diode test + hardware trial, NOT a positive-supply pin despite the usual convention on similar modules |

Touch panel (4-wire resistive, physically fused to RS/WR/DB6/DB7 on this
module -- not a separate connector) is wired but not read by firmware; see
`synora_display.h` for why (ESP32-S3's LCD_CAM peripheral owns those 4 pins
exclusively once the display bus initializes, and those specific GPIOs are
ADC2 channels, which conflicts with Wi-Fi).

### Gotchas that cost real debugging time

- **GPIO19/20 are the ESP32-S3's fixed native-USB D-/D+ pins.** An earlier
  revision put D5/D6 there. The moment the display's I80 bus initialized, it
  fought the native USB PHY for those pins and the board crashed and
  dropped off USB entirely (had to re-enter download mode to reflash every
  time). Do not use GPIO19/20 for anything else on this board.
- **GPIO22-25 don't exist on the ESP32-S3 at all** (valid range is 0-21 and
  26-48). An earlier revision assigned PUMP_GPIO/BUILTIN_LED_GPIO to 22/23;
  it silently went undetected for a while because nothing in the codebase
  actually referenced those macros yet, so the compiler never evaluated the
  invalid enum value. First real use failed with `'GPIO_NUM_22' was not
  declared in this scope`.
- **Backlight (LED) is active-low on this specific module** -- wiring it to
  3.3V (the usual convention) left it permanently dark even with correct
  voltage confirmed present at the pin. Tie it to GND instead.
- **RD isn't for touch.** It's the LCD controller's read strobe, unrelated to
  the touch panel's X/Y wires; tie it to 3.3V regardless of touch wiring.
- **The VL53L1X water-level sensor is on its own I2C bus, not sharing SHT31's.**
  The `saleca/vl53l1x_uld_esp_wrapper` component creates its own internal I2C
  bus handle from raw pins and can't be handed a pre-existing shared bus, so
  it gets I2C_NUM_1 (GPIO2/4) instead of sharing I2C_NUM_0 with SHT31.

## Other board pins

| Function | GPIO(s) | Notes |
|---|---|---|
| BOOT button / factory reset + voice toggle | GPIO0 | strapping pin, standard BOOT button location; the only physical button on this board -- 10s hold = factory reset, click = toggle voice session |
| Soil moisture ADC | GPIO1 | ADC1_CH0 |
| I2C bus 0 (SHT31) | SDA=GPIO9, SCL=GPIO8 | I2C_NUM_0 |
| I2C bus 1 (VL53L1X water level) | SDA=GPIO2, SCL=GPIO4 | I2C_NUM_1 -- separate bus, not shared with SHT31; see gotchas below |
| Speaker I2S (MAX98357A) | DOUT=GPIO5, BCLK=GPIO6, LRCK=GPIO7 | |
| Mic I2S (INMP441) | WS=GPIO41, SCK=GPIO39, DIN=GPIO40 | |
| Water pump (IRLZ44N gate) | GPIO47 | active-high |
| Built-in LED | GPIO48 | |

Reserved, do not use for anything: GPIO3/45/46 (strapping), GPIO19/20
(native USB D-/D+), GPIO22-25 (don't exist on ESP32-S3), GPIO26-32 (SPI
flash), GPIO33-37 (octal PSRAM), GPIO43/44 (console UART).

No free GPIOs remain in the low range at this point (GPIO2/4, formerly free,
now carry the VL53L1X's dedicated I2C bus).
