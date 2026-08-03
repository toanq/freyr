#ifndef AFE_ENGINE_CONFIG_H
#define AFE_ENGINE_CONFIG_H

// Phase-1 build knobs for the AFE audio engine (Synora greenfield firmware).
//
// The engine was copied from xiaozhi's afe_audio_engine with two optional paths
// gated off so Phase 1 depends ONLY on esp-sr (no opus/esp_mn/assets):
//
//   SYNORA_ENABLE_MULTINET     - on-device MultiNet command recognition
//                                (CustomWakeWord). Pulls in esp_mn + the Assets
//                                system. Deferred; WakeNet handles wake-up.
//   SYNORA_SEND_WAKE_WORD_DATA - Opus-encode the buffered wake-word audio to
//                                upload to the server. Belongs with the protocol
//                                work (Phase 2); pulls in esp_audio_codec.
//
// Flip either to 1 (and add the matching component/source) to re-enable.
#define SYNORA_ENABLE_MULTINET 0
#define SYNORA_SEND_WAKE_WORD_DATA 0

#endif  // AFE_ENGINE_CONFIG_H
