#include "test_util.h"

#include "../main/boards/synora/dashboard_status.h"

int RunDashboardStatusTests() {
    TEST_SUITE_BEGIN();

    // Invalid readings always render as the neutral/unknown tile, regardless
    // of whatever garbage value happens to be alongside `valid=false`.
    {
        TEST_CHECK(SoilMoistureTileStatus(false, 99.0f) == TileStatus::kUnknown);
        TEST_CHECK(WaterLevelTileStatus(false, -5.0f) == TileStatus::kUnknown);
        TEST_CHECK(TemperatureTileStatus(false, 22.0f) == TileStatus::kUnknown);
        TEST_CHECK(HumidityTileStatus(false, 50.0f) == TileStatus::kUnknown);
    }

    // Soil moisture: one-sided bad band (dry side only).
    {
        TEST_CHECK(SoilMoistureTileStatus(true, 5.0f) == TileStatus::kBad);
        TEST_CHECK(SoilMoistureTileStatus(true, 19.9f) == TileStatus::kBad);
        TEST_CHECK(SoilMoistureTileStatus(true, 20.0f) == TileStatus::kWarn);
        TEST_CHECK(SoilMoistureTileStatus(true, 39.9f) == TileStatus::kWarn);
        TEST_CHECK(SoilMoistureTileStatus(true, 40.0f) == TileStatus::kGood);
        TEST_CHECK(SoilMoistureTileStatus(true, 100.0f) == TileStatus::kGood);
    }

    // Water level: one-sided bad band (low side only).
    {
        TEST_CHECK(WaterLevelTileStatus(true, 0.0f) == TileStatus::kBad);
        TEST_CHECK(WaterLevelTileStatus(true, 14.9f) == TileStatus::kBad);
        TEST_CHECK(WaterLevelTileStatus(true, 15.0f) == TileStatus::kWarn);
        TEST_CHECK(WaterLevelTileStatus(true, 34.9f) == TileStatus::kWarn);
        TEST_CHECK(WaterLevelTileStatus(true, 35.0f) == TileStatus::kGood);
    }

    // Temperature: bad/warn on both tails.
    {
        TEST_CHECK(TemperatureTileStatus(true, 10.0f) == TileStatus::kBad);
        TEST_CHECK(TemperatureTileStatus(true, 40.0f) == TileStatus::kBad);
        TEST_CHECK(TemperatureTileStatus(true, 16.0f) == TileStatus::kWarn);
        TEST_CHECK(TemperatureTileStatus(true, 32.0f) == TileStatus::kWarn);
        TEST_CHECK(TemperatureTileStatus(true, 22.0f) == TileStatus::kGood);
    }

    // Humidity: bad/warn on both tails.
    {
        TEST_CHECK(HumidityTileStatus(true, 10.0f) == TileStatus::kBad);
        TEST_CHECK(HumidityTileStatus(true, 90.0f) == TileStatus::kBad);
        TEST_CHECK(HumidityTileStatus(true, 35.0f) == TileStatus::kWarn);
        TEST_CHECK(HumidityTileStatus(true, 75.0f) == TileStatus::kWarn);
        TEST_CHECK(HumidityTileStatus(true, 55.0f) == TileStatus::kGood);
    }

    TEST_SUITE_END();
}
