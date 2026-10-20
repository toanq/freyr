namespace Synora.Core;

// One uploaded firmware .bin (admin-only upload via POST /firmware), served
// back to devices at GET /firmware/{id}/binary for both the WiFi-OTA
// (commands/ota MQTT trigger) and BLE-push fallback paths. Same
// file-on-disk-plus-path-column convention as Device.AvatarPath /
// MicTestUpload.FilePath, stored under Firmware:StoragePath.
public class FirmwareBuild
{
    public Guid Id { get; set; }
    public required string Version { get; set; }

    // Filename (not a full path) of the .bin on disk under Firmware:StoragePath.
    public required string FileName { get; set; }

    // Computed server-side on upload. Sent to the device in the ota command
    // payload/ota_start control write so it can verify the image it
    // downloaded/received before flashing it.
    public required string Sha256 { get; set; }
    public long SizeBytes { get; set; }
    public DateTimeOffset CreatedAt { get; set; }
    public string? Notes { get; set; }

    // True when uploaded as a flash-kit zip (bootloader, partition table,
    // otadata, app, srmodels) for USB flashing from the web console; the kit
    // lives under Firmware:StoragePath/<Id>/.
    public bool HasFlashKit { get; set; }
}
