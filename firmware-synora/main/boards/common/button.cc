#include "button.h"

Button::Button(gpio_num_t gpio_num, bool active_high, uint16_t long_press_time) {
    if (gpio_num == GPIO_NUM_NC) {
        return;
    }
    button_config_t button_config = {
        .long_press_time = long_press_time,
        .short_press_time = 0
    };
    button_gpio_config_t gpio_config = {
        .gpio_num = gpio_num,
        .active_level = static_cast<uint8_t>(active_high ? 1 : 0),
        .enable_power_save = false,
        .disable_pull = false
    };
    ESP_ERROR_CHECK(iot_button_new_gpio_device(&button_config, &gpio_config, &button_handle_));
}

Button::~Button() {
    if (button_handle_ != nullptr) {
        iot_button_delete(button_handle_);
    }
}

void Button::OnLongPress(std::function<void()> callback) {
    if (button_handle_ == nullptr) {
        return;
    }
    on_long_press_ = callback;
    iot_button_register_cb(button_handle_, BUTTON_LONG_PRESS_START, nullptr, [](void* handle, void* usr_data) {
        auto* button = static_cast<Button*>(usr_data);
        if (button->on_long_press_) {
            button->on_long_press_();
        }
    }, this);
}

void Button::OnClick(std::function<void()> callback) {
    if (button_handle_ == nullptr) {
        return;
    }
    on_click_ = callback;
    iot_button_register_cb(button_handle_, BUTTON_SINGLE_CLICK, nullptr, [](void* handle, void* usr_data) {
        auto* button = static_cast<Button*>(usr_data);
        if (button->on_click_) {
            button->on_click_();
        }
    }, this);
}
