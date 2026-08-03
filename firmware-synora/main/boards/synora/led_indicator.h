#pragma once

#include <atomic>

#include <driver/gpio.h>

// Drives config.h's BUILTIN_LED_GPIO for the "find my device" MQTT ping
// command (synora_mqtt.cc's "commands/ping" topic, see synora_board.cc) --
// this board has no buzzer, so a blink pattern is the whole reaction there
// is.
class LedIndicator {
public:
    explicit LedIndicator(gpio_num_t gpio);

    // Blinks `times` on/off cycles on a dedicated short-lived task, so the
    // MQTT event handler that calls this doesn't block for the whole
    // pattern. A ping that arrives while a previous one is still blinking
    // is dropped rather than queued/restarted -- the device is already
    // visibly signaling, there's nothing more to show.
    void Blink(int times);

    LedIndicator(const LedIndicator&) = delete;
    LedIndicator& operator=(const LedIndicator&) = delete;

private:
    struct BlinkArgs {
        LedIndicator* self;
        int times;
    };
    static void BlinkTask(void* arg);

    gpio_num_t gpio_;
    std::atomic<bool> blinking_ = false;
};
