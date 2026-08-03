#include "sht31_sensor.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "Sht31Sensor";

namespace {
// CRC-8, polynomial 0x31, init 0xFF -- SHT3x datasheet section 4.12.
uint8_t Crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31) : static_cast<uint8_t>(crc << 1);
        }
    }
    return crc;
}

bool ChecksumValid(uint8_t msb, uint8_t lsb, uint8_t crc) {
    uint8_t buf[2] = {msb, lsb};
    return Crc8(buf, sizeof(buf)) == crc;
}
}  // namespace

Sht31Sensor::Sht31Sensor(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {}

bool Sht31Sensor::Read(float& temperature_celsius, float& humidity_percent) {
    // Single-shot, high repeatability, clock stretching disabled (datasheet
    // table 8): command 0x2400, sent MSB-first like every SHT3x command.
    uint8_t cmd[2] = {0x24, 0x00};
    if (i2c_master_transmit(i2c_device_, cmd, sizeof(cmd), 100) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send measurement command");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));  // high-repeatability max duration is ~15ms; some margin

    uint8_t data[6];
    if (i2c_master_receive(i2c_device_, data, sizeof(data), 100) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read measurement");
        return false;
    }

    if (!ChecksumValid(data[0], data[1], data[2]) || !ChecksumValid(data[3], data[4], data[5])) {
        ESP_LOGW(TAG, "Checksum mismatch");
        return false;
    }

    uint16_t raw_temp = static_cast<uint16_t>((data[0] << 8) | data[1]);
    uint16_t raw_humidity = static_cast<uint16_t>((data[3] << 8) | data[4]);
    temperature_celsius = -45.0f + 175.0f * (static_cast<float>(raw_temp) / 65535.0f);
    humidity_percent = 100.0f * (static_cast<float>(raw_humidity) / 65535.0f);
    return true;
}
