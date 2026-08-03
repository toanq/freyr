#include "led_indicator.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
constexpr int kOnMs = 350;
constexpr int kOffMs = 250;
}  // namespace

LedIndicator::LedIndicator(gpio_num_t gpio) : gpio_(gpio) {
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << gpio_;
    config.mode = GPIO_MODE_OUTPUT;
    gpio_config(&config);
    gpio_set_level(gpio_, 0);
}

void LedIndicator::Blink(int times) {
    bool already_blinking = blinking_.exchange(true);
    if (already_blinking) {
        return;
    }
    auto* args = new BlinkArgs{this, times};
    xTaskCreate(BlinkTask, "synora_led_blink", 2048, args, 3, nullptr);
}

void LedIndicator::BlinkTask(void* arg) {
    auto* args = static_cast<BlinkArgs*>(arg);
    LedIndicator* self = args->self;
    int times = args->times;
    delete args;

    for (int i = 0; i < times; i++) {
        gpio_set_level(self->gpio_, 1);
        vTaskDelay(pdMS_TO_TICKS(kOnMs));
        gpio_set_level(self->gpio_, 0);
        vTaskDelay(pdMS_TO_TICKS(kOffMs));
    }
    self->blinking_.store(false);
    vTaskDelete(nullptr);
}
