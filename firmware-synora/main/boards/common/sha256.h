#pragma once

#include <cstddef>
#include <cstdint>

// Minimal, dependency-free streaming SHA-256 (FIPS 180-4) used only to
// integrity-check a BLE-pushed firmware image (see ble_provisioning.cc's
// ota_start/firmware-chunk/ota_commit handlers). Vendored instead of using
// mbedtls's legacy `mbedtls/sha256.h` because ESP-IDF v6's mbedtls component
// restructured around the PSA Crypto API and dropped that public header
// (and `psa/crypto.h`'s include path isn't exposed to component consumers
// either) -- a few dozen lines of self-contained SHA-256 sidesteps that
// churn entirely for this single, isolated use.
struct Sha256Context {
    uint32_t state[8];
    uint64_t total_len = 0;
    uint8_t buffer[64];
    size_t buffer_len = 0;
};

void Sha256Init(Sha256Context& ctx);
void Sha256Update(Sha256Context& ctx, const uint8_t* data, size_t len);
void Sha256Final(Sha256Context& ctx, uint8_t digest[32]);
