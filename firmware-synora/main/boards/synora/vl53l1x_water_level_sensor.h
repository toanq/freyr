#pragma once

#include "vl53l1x.h"

// Wraps the VL53L1X time-of-flight sensor for water-tank level sensing. Mount
// the sensor at the top of the tank facing down at the water surface -- a
// SHORTER measured distance means a HIGHER water level. Owns its own I2C
// port/bus (see config.h's WATER_LEVEL_I2C_* comment for why it can't share
// a pre-existing bus handle).
//
// empty_distance_mm/full_distance_mm are the sensor readings expected at an
// empty and a full tank respectively -- these depend on the physical mounting
// height and must be calibrated per enclosure; the defaults used by callers
// are placeholders.
class WaterLevelSensor {
public:
    WaterLevelSensor(gpio_num_t sda_gpio, gpio_num_t scl_gpio, i2c_port_num_t i2c_port,
                      uint16_t empty_distance_mm, uint16_t full_distance_mm);

    // Returns 0 (empty) - 100 (full), clamped. Returns false if the sensor
    // never initialized successfully.
    bool ReadPercent(float& percent);

private:
    vl53l1x_i2c_handle_t i2c_handle_ = VL53L1X_I2C_INIT;
    vl53l1x_handle_t handle_ = VL53L1X_INIT;
    vl53l1x_device_handle_t device_ = VL53L1X_DEVICE_INIT;
    bool ready_ = false;
    uint16_t empty_distance_mm_;
    uint16_t full_distance_mm_;
};
