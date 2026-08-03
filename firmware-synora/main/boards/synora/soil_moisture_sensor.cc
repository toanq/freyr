#include "soil_moisture_sensor.h"

#include <algorithm>

#include <esp_log.h>

static const char* TAG = "SoilMoistureSensor";

namespace {
// PLACEHOLDER, uncalibrated for this capacitive module -- borrowed from a
// resistive LM393 module's typical 3.3V output range. Measure the real
// millivolt readings in open air (dry) and fully wet soil and replace these
// before trusting the reported percentage.
constexpr int kDryMillivolts = 2700;
constexpr int kWetMillivolts = 1200;
}  // namespace

SoilMoistureSensor::SoilMoistureSensor(adc_unit_t unit, adc_channel_t channel) : channel_(channel) {
    adc_oneshot_unit_init_cfg_t init_config = {};
    init_config.unit_id = unit;
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle_));

    adc_oneshot_chan_cfg_t chan_config = {};
    chan_config.atten = ADC_ATTEN_DB_12;
    chan_config.bitwidth = ADC_BITWIDTH_DEFAULT;
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle_, channel_, &chan_config));

    adc_cali_curve_fitting_config_t cali_config = {};
    cali_config.unit_id = unit;
    cali_config.chan = channel_;
    cali_config.atten = ADC_ATTEN_DB_12;
    cali_config.bitwidth = ADC_BITWIDTH_DEFAULT;
    calibrated_ = adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle_) == ESP_OK;
    if (!calibrated_) {
        ESP_LOGW(TAG, "ADC calibration unavailable, falling back to raw counts");
    }
}

SoilMoistureSensor::~SoilMoistureSensor() {
    if (cali_handle_ != nullptr) {
        adc_cali_delete_scheme_curve_fitting(cali_handle_);
    }
    if (adc_handle_ != nullptr) {
        adc_oneshot_del_unit(adc_handle_);
    }
}

bool SoilMoistureSensor::ReadPercent(float& percent) {
    int raw = 0;
    if (adc_oneshot_read(adc_handle_, channel_, &raw) != ESP_OK) {
        ESP_LOGW(TAG, "ADC read failed");
        return false;
    }

    int millivolts;
    if (calibrated_) {
        if (adc_cali_raw_to_voltage(cali_handle_, raw, &millivolts) != ESP_OK) {
            ESP_LOGW(TAG, "ADC calibration conversion failed");
            return false;
        }
    } else {
        // ADC_BITWIDTH_DEFAULT on ESP32-S3 is 12 bits; ADC_ATTEN_DB_12 covers ~0-3.3V.
        millivolts = raw * 3300 / 4095;
    }

    float ratio = static_cast<float>(kDryMillivolts - millivolts) / static_cast<float>(kDryMillivolts - kWetMillivolts);
    percent = std::clamp(ratio, 0.0f, 1.0f) * 100.0f;
    return true;
}
