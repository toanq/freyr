#pragma once

#include <driver/gpio.h>

// Bit-banged single-wire DHT11 temperature/humidity sensor. No suitable
// ESP-IDF managed component exists for plain DHT11 (checked the registry) --
// same reasoning as synora_display.cc's hand-rolled ST7793 driver: this
// hand-rolls the datasheet's protocol (host pulls the line low to start,
// sensor acks, then clocks out 40 bits encoded as pulse width) directly
// against driver/gpio.h + esp_timer, rather than depending on an unverified
// third-party component for a handful of lines of bit-banging.
class Dht11Sensor {
public:
    explicit Dht11Sensor(gpio_num_t data_gpio);

    // Returns false (leaving both outputs unchanged) on a timeout waiting for
    // the sensor's response/bits, or a checksum mismatch. DHT11 itself only
    // samples internally at most once per ~1s -- callers on a faster cadence
    // will just see failures until the sensor's own next sample is ready,
    // same Read()->bool convention as this board's other sensors.
    bool Read(float& temperature_celsius, float& humidity_percent);

private:
    // Blocks (with a timeout) until data_gpio_ reads `level`, returning the
    // microseconds spent waiting, or -1 on timeout. The core building block
    // both the ack sequence and each bit's pulse-width measurement are built
    // from.
    int WaitForLevel(int level, int timeout_us) const;

    gpio_num_t data_gpio_;
};
