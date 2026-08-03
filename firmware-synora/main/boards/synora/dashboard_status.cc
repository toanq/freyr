#include "dashboard_status.h"

TileStatus SoilMoistureTileStatus(bool valid, float percent) {
    if (!valid) {
        return TileStatus::kUnknown;
    }
    if (percent < 20.0f) {
        return TileStatus::kBad;
    }
    if (percent < 40.0f) {
        return TileStatus::kWarn;
    }
    return TileStatus::kGood;
}

TileStatus WaterLevelTileStatus(bool valid, float percent) {
    if (!valid) {
        return TileStatus::kUnknown;
    }
    if (percent < 15.0f) {
        return TileStatus::kBad;
    }
    if (percent < 35.0f) {
        return TileStatus::kWarn;
    }
    return TileStatus::kGood;
}

TileStatus TemperatureTileStatus(bool valid, float celsius) {
    if (!valid) {
        return TileStatus::kUnknown;
    }
    if (celsius < 15.0f || celsius > 35.0f) {
        return TileStatus::kBad;
    }
    if (celsius < 18.0f || celsius > 30.0f) {
        return TileStatus::kWarn;
    }
    return TileStatus::kGood;
}

TileStatus HumidityTileStatus(bool valid, float percent) {
    if (!valid) {
        return TileStatus::kUnknown;
    }
    if (percent < 30.0f || percent > 80.0f) {
        return TileStatus::kBad;
    }
    if (percent < 40.0f || percent > 70.0f) {
        return TileStatus::kWarn;
    }
    return TileStatus::kGood;
}
