namespace Synora.Core;

// Single-row table (Id is always 1) for global, admin-editable config that
// isn't tied to any one device/user -- see Program.cs's GET/PUT
// /admin/system-settings. Extend by adding more nullable/defaulted
// properties + a migration, same pattern as every other entity here;
// deliberately not a generic key-value store, to match how the rest of
// this codebase does typed config (LlmOptions/AsrOptions/TtsOptions etc.).
public class SystemSettings
{
    public int Id { get; set; }

    // Preset voice id passed to the TTS sidecar's /synthesize call (see
    // VieNeuTtsClient) -- one of VieNeu-TTS v3 Turbo's 14 built-in voices
    // (GET /admin/tts-voices lists them all). "Trúc Ly" is a natural-
    // sounding female voice, picked as the default after the previous
    // backend (facebook/mms-tts-vie, single-speaker, no voice choice) was
    // replaced specifically for this reason.
    public string TtsVoice { get; set; } = "Trúc Ly";
}
