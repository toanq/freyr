namespace Synora.Core;

// One WAV file captured by the firmware's diagnostic `mictest` console
// command and published over MQTT (synora/devices/{deviceId}/mictest/upload
// -- see MqttService), so an admin can browse/play back mic recordings taken
// directly on the hardware without needing physical access to the device.
// Same file-on-disk-plus-path-column convention as Device.AvatarPath /
// DiscussionMessage.UserAudioPath, stored under MicTestUploads:StoragePath.
public class MicTestUpload
{
    public Guid Id { get; set; }
    public Guid DeviceId { get; set; }
    public Device? Device { get; set; }

    // Filename (not a full path) of the WAV file on disk under
    // MicTestUploads:StoragePath.
    public required string FilePath { get; set; }
    public long SizeBytes { get; set; }
    public DateTimeOffset CreatedAt { get; set; }
}
