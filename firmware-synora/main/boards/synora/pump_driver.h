#pragma once

#include <driver/gpio.h>
#include <esp_timer.h>

// Drives the water pump's MOSFET gate (config.h's PUMP_GPIO, active-high).
// RunFor() turns the gate on and schedules an esp_timer one-shot to turn it
// back off, so the MQTT event handler that calls it (SynoraBoard's
// "commands/water" subscription, see synora_board.cc) never blocks for the
// whole pulse duration.
class PumpDriver {
public:
    explicit PumpDriver(gpio_num_t gpio);

    // Starts a pump pulse for duration_seconds, clamped to
    // [0, kMaxDurationSeconds] -- the duration comes straight off an MQTT
    // command payload from the server, so it's validated here rather than
    // trusted. A command arriving while a previous pulse is still running
    // restarts the stop timer from now (extends/resets, doesn't stack).
    // Returns the actual (clamped) seconds the pump will run for, 0 if the
    // request was ignored -- callers that want to mirror the pulse
    // elsewhere (e.g. SynoraDisplay's watering animation) use this rather
    // than re-deriving/duplicating the clamp themselves.
    int RunFor(int duration_seconds);

    // For self.get_device_status (MCP tool) -- true between a RunFor() call
    // and its scheduled StopCallback().
    bool IsRunning() const { return running_; }

    PumpDriver(const PumpDriver&) = delete;
    PumpDriver& operator=(const PumpDriver&) = delete;

private:
    static void StopCallback(void* arg);

    gpio_num_t gpio_;
    esp_timer_handle_t stop_timer_ = nullptr;
    bool running_ = false;
};
