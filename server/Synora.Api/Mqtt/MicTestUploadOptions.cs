namespace Synora.Api.Mqtt;

/// <summary>
/// Config for where MqttService writes WAV files published by the firmware's
/// diagnostic `mictest` console command (synora/devices/{deviceId}/mictest/upload),
/// bound from the "MicTestUploads" configuration section. Same file-on-disk-
/// plus-path-column convention as Recordings:StoragePath/Avatar:StoragePath,
/// kept as its own directory since these are hardware-diagnostic recordings,
/// not conversation history.
/// </summary>
public sealed class MicTestUploadOptions
{
    public string StoragePath { get; set; } = "/data/mictest";

    public int MaxPayloadBytes { get; set; } = 2 * 1024 * 1024;
}
