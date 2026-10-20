#include "dht11_sensor.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "Dht11Sensor";

namespace {
// Generous relative to the datasheet's own bit-timing (low ~50us, high
// ~26-28us for a 0 / ~70us for a 1) and ack sequence (~80us pulses) -- just
// needs to be well past the longest real pulse without waiting so long a
// genuinely stuck line blocks the caller's task for an unreasonable time.
constexpr int kTimeoutUs = 200;

// Splits cleanly between a "0" bit's ~26-28us high pulse and a "1" bit's
// ~70us one.
constexpr int kBitThresholdUs = 40;
}  // namespace

Dht11Sensor::Dht11Sensor(gpio_num_t data_gpio) : data_gpio_(data_gpio) {}

int Dht11Sensor::WaitForLevel(int level, int timeout_us) const {
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(data_gpio_) != level) {
        if (esp_timer_get_time() - start > timeout_us) {
            return -1;
        }
    }
    return static_cast<int>(esp_timer_get_time() - start);
}

bool Dht11Sensor::Read(float& temperature_celsius, float& humidity_percent) {
    // Start signal: host pulls the single-wire line low for >=18ms (open-
    // drain -- an external/inline pull-up on the module is what holds it
    // high the rest of the time), then releases it and waits for the
    // sensor's response.
    gpio_config_t out_config = {};
    out_config.pin_bit_mask = 1ULL << data_gpio_;
    out_config.mode = GPIO_MODE_OUTPUT_OD;
    out_config.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&out_config);

    gpio_set_level(data_gpio_, 0);
    // datasheet: >=18ms. +1 tick: at 100 Hz a 2-tick delay can end ~10ms
    // after it starts (partial first tick).
    vTaskDelay(pdMS_TO_TICKS(20) + 1);
    gpio_set_level(data_gpio_, 1);  // release -- pull-up brings it back high

    gpio_config_t in_config = {};
    in_config.pin_bit_mask = 1ULL << data_gpio_;
    in_config.mode = GPIO_MODE_INPUT;
    in_config.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&in_config);

    // From here the whole transaction is ~4-5ms and microsecond-timing-
    // sensitive -- interrupts on this core stay off for all of it so a tick
    // ISR landing mid-pulse can't corrupt a width measurement. Every exit
    // path below re-enables interrupts before returning.
    portDISABLE_INTERRUPTS();

    // Sensor's ack: pulls low ~80us, then high ~80us, before the first data bit.
    if (WaitForLevel(0, kTimeoutUs) < 0) {
        portENABLE_INTERRUPTS();
        ESP_LOGW(TAG, "No response (ack low)");
        return false;
    }
    if (WaitForLevel(1, kTimeoutUs) < 0) {
        portENABLE_INTERRUPTS();
        ESP_LOGW(TAG, "No response (ack high)");
        return false;
    }
    if (WaitForLevel(0, kTimeoutUs) < 0) {
        portENABLE_INTERRUPTS();
        ESP_LOGW(TAG, "Ack never ended");
        return false;
    }

    // 40 bits: humidity int, humidity frac, temp int, temp frac, checksum --
    // each bit is a ~50us low pulse followed by a high pulse whose width
    // encodes the bit value (see kBitThresholdUs).
    uint8_t data[5] = {0, 0, 0, 0, 0};
    for (int i = 0; i < 40; i++) {
        if (WaitForLevel(1, kTimeoutUs) < 0) {
            portENABLE_INTERRUPTS();
            ESP_LOGW(TAG, "Bit %d: low phase never ended", i);
            return false;
        }
        int high_us = WaitForLevel(0, kTimeoutUs);
        if (high_us < 0) {
            portENABLE_INTERRUPTS();
            ESP_LOGW(TAG, "Bit %d: high phase never ended", i);
            return false;
        }
        data[i / 8] = static_cast<uint8_t>((data[i / 8] << 1) | (high_us > kBitThresholdUs ? 1 : 0));
    }

    portENABLE_INTERRUPTS();

    uint8_t checksum = static_cast<uint8_t>(data[0] + data[1] + data[2] + data[3]);
    if (checksum != data[4]) {
        ESP_LOGW(TAG, "Checksum mismatch: got 0x%02x, expected 0x%02x", data[4], checksum);
        return false;
    }

    // Plain DHT11 (unlike the DHT22) only has integer resolution -- the
    // "frac" bytes are conventionally 0 (some clones repurpose them, but
    // there's no reliable cross-clone way to interpret that, so this reads
    // the standard/documented behavior only).
    humidity_percent = static_cast<float>(data[0]);
    temperature_celsius = static_cast<float>(data[2]);
    return true;
}
