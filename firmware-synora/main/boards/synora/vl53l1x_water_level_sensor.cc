#include "vl53l1x_water_level_sensor.h"

#include <algorithm>

#include <esp_log.h>

static const char* TAG = "WaterLevelSensor";

WaterLevelSensor::WaterLevelSensor(gpio_num_t sda_gpio, gpio_num_t scl_gpio, i2c_port_num_t i2c_port,
                                    uint16_t empty_distance_mm, uint16_t full_distance_mm)
    : empty_distance_mm_(empty_distance_mm), full_distance_mm_(full_distance_mm) {
    i2c_handle_.sda_gpio = sda_gpio;
    i2c_handle_.scl_gpio = scl_gpio;
    i2c_handle_.i2c_port = i2c_port;

    handle_.i2c_handle = &i2c_handle_;
    if (!vl53l1x_init(&handle_)) {
        ESP_LOGW(TAG, "VL53L1X initialization failed");
        return;
    }

    device_.vl53l1x_handle = &handle_;
    if (!vl53l1x_add_device(&device_)) {
        ESP_LOGW(TAG, "VL53L1X device add failed (addr 0x%02x)", device_.i2c_address);
        return;
    }

    ready_ = true;
}

bool WaterLevelSensor::ReadPercent(float& percent) {
    if (!ready_ || !handle_.initialized) {
        return false;
    }

    uint16_t distance_mm = vl53l1x_get_mm(&device_);
    float span = static_cast<float>(empty_distance_mm_) - static_cast<float>(full_distance_mm_);
    float ratio = (static_cast<float>(empty_distance_mm_) - static_cast<float>(distance_mm)) / span;
    percent = std::clamp(ratio, 0.0f, 1.0f) * 100.0f;
    return true;
}
