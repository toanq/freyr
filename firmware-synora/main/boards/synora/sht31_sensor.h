#pragma once

#include "boards/common/i2c_device.h"

// Sensirion SHT3x (SHT31) temperature/humidity sensor over I2C. Single-shot,
// high-repeatability, no clock stretching -- simplest mode, and clock
// stretching support is inconsistent across I2C master implementations.
class Sht31Sensor : public I2cDevice {
public:
    Sht31Sensor(i2c_master_bus_handle_t i2c_bus, uint8_t addr);

    // Returns false (leaving both outputs unchanged) on I2C failure or a
    // failed CRC-8 checksum on either half of the reading.
    bool Read(float& temperature_celsius, float& humidity_percent);
};
