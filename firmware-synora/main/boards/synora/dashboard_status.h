#pragma once

// Pure threshold logic, no ESP-IDF/hardware dependency -- kept separate from
// synora_display.cc (which owns color choices/drawing) specifically so it's
// host-testable, same reasoning as eyes_geometry.h/sensor_readings.h.
//
// Maps a sensor reading to a coarse status used to color its dashboard tile.
// Thresholds are a first-pass, general "houseplant + human-comfortable room"
// guess, not derived from any specific plant species -- easy to retune in
// one place (this file) once real usage shows they're off for a given plant.
enum class TileStatus {
    kUnknown,  // reading not valid yet -- rendered as a neutral/gray tile
    kGood,
    kWarn,
    kBad,
};

// Soil moisture: dry soil is the more urgent failure mode (a wilting/dying
// plant) than "too wet", so the bad band is one-sided.
TileStatus SoilMoistureTileStatus(bool valid, float percent);

// Water tank level: low tank means the next scheduled/manual watering can't
// actually run, so (like soil) the bad band is the low side.
TileStatus WaterLevelTileStatus(bool valid, float percent);

// Air temperature (Celsius) -- both too cold and too hot are bad for most
// houseplants and for the device's own electronics, so both tails matter.
TileStatus TemperatureTileStatus(bool valid, float celsius);

// Air humidity (percent) -- both too dry and too humid are outside a
// comfortable band, so both tails matter (same shape as temperature).
TileStatus HumidityTileStatus(bool valid, float percent);
