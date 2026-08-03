#pragma once

#include <hal/adc_types.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_oneshot.h>

// Reads a capacitive soil moisture module's analog output. Calibrate
// kDryMillivolts/kWetMillivolts (soil_moisture_sensor.cc) against the actual
// module in open air / fully wet soil -- the values there are placeholders
// carried over from a different (resistive LM393) module and are very
// likely wrong for this capacitive one; a capacitive sensor's voltage-vs-
// moisture curve doesn't necessarily even run the same direction.
class SoilMoistureSensor {
public:
    SoilMoistureSensor(adc_unit_t unit, adc_channel_t channel);
    ~SoilMoistureSensor();

    // Returns 0 (fully dry) - 100 (fully saturated), clamped. Returns false
    // (leaving percent unchanged) if the ADC read itself fails.
    bool ReadPercent(float& percent);

private:
    adc_channel_t channel_;
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    adc_cali_handle_t cali_handle_ = nullptr;
    bool calibrated_ = false;
};
