#include "pump_driver.h"

#include <algorithm>

#include <esp_log.h>

namespace {
// Safety cap on any single pulse regardless of what the server sends --
// a stuck/misconfigured schedule or a bad payload shouldn't be able to run
// the pump indefinitely.
constexpr int kMaxDurationSeconds = 300;
}  // namespace

static const char* TAG = "PumpDriver";

PumpDriver::PumpDriver(gpio_num_t gpio) : gpio_(gpio) {
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << gpio_;
    config.mode = GPIO_MODE_OUTPUT;
    gpio_config(&config);
    gpio_set_level(gpio_, 0);

    esp_timer_create_args_t timer_args = {};
    timer_args.callback = &PumpDriver::StopCallback;
    timer_args.arg = this;
    timer_args.name = "pump_stop";
    esp_timer_create(&timer_args, &stop_timer_);
}

int PumpDriver::RunFor(int duration_seconds) {
    int clamped = std::clamp(duration_seconds, 0, kMaxDurationSeconds);
    if (clamped <= 0) {
        ESP_LOGW(TAG, "Ignoring water command with non-positive duration (%d s)", duration_seconds);
        return 0;
    }
    if (clamped != duration_seconds) {
        ESP_LOGW(TAG, "Clamping requested duration %d s to %d s", duration_seconds, clamped);
    }

    // esp_timer_stop() is a no-op if the timer isn't currently running --
    // safe to call unconditionally rather than tracking "is it running"
    // ourselves.
    esp_timer_stop(stop_timer_);
    gpio_set_level(gpio_, 1);
    running_ = true;
    ESP_LOGI(TAG, "Pump ON for %d s", clamped);
    esp_timer_start_once(stop_timer_, static_cast<uint64_t>(clamped) * 1000000ULL);
    return clamped;
}

void PumpDriver::StopCallback(void* arg) {
    auto* self = static_cast<PumpDriver*>(arg);
    gpio_set_level(self->gpio_, 0);
    self->running_ = false;
    ESP_LOGI(TAG, "Pump OFF");
}
