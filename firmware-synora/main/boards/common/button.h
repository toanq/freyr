#ifndef BUTTON_H_
#define BUTTON_H_

#include <driver/gpio.h>
#include <iot_button.h>
#include <button_types.h>
#include <button_gpio.h>
#include <functional>

// Thin wrapper around the espressif/button (iot_button) component.
// long_press_time (ms) is the single hold threshold that fires OnLongPress.
class Button {
public:
    Button(gpio_num_t gpio_num, bool active_high, uint16_t long_press_time);
    ~Button();

    void OnLongPress(std::function<void()> callback);
    void OnClick(std::function<void()> callback);

private:
    button_handle_t button_handle_ = nullptr;
    std::function<void()> on_long_press_;
    std::function<void()> on_click_;
};

#endif // BUTTON_H_
