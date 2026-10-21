using Microsoft.EntityFrameworkCore;
using Synora.Api.Mqtt;
using Synora.Core;
using Synora.Infrastructure;

namespace Synora.Api.Devices;

// Owner changes shared by DELETE /devices/{id} (unpair) and the admin owner
// endpoint, so an admin assignment/clear has exactly the side effects of a
// pairing/unpairing: the previous owner's settings are dropped and the
// device's retained MQTT state (isPaired) is refreshed.
internal static class DeviceOwnership
{
    // Clears the previous owner's personalization -- a device sitting
    // unpaired/re-pairable in the system (or handed to someone else) shouldn't
    // keep showing whoever had it last's chosen photo. Watering schedules and
    // low-water alert settings are that owner's choices too, so they're
    // dropped rather than inherited; conversation history stays with the
    // device (admins can still read it). Not saved here -- callers save once.
    public static async Task ClearOwnerSettingsAsync(SynoraDbContext db, Device device, string avatarStoragePath)
    {
        if (device.AvatarPath is not null)
        {
            var avatarPath = Path.Combine(avatarStoragePath, device.AvatarPath);
            if (File.Exists(avatarPath))
            {
                File.Delete(avatarPath);
            }
            device.AvatarPath = null;
        }

        var oldSchedules = await db.WateringSchedules.Where(s => s.DeviceId == device.Id).ToListAsync();
        db.WateringSchedules.RemoveRange(oldSchedules);
        device.LowWaterWarningThresholdPercent = null;
        device.LowWaterCriticalThresholdPercent = null;
        device.LowWaterAlertIntervalMinutes = null;
        device.LowWaterLastAlertedAt = null;
    }

    // Unpair-then-pair when reassigning; no-op when the owner doesn't change.
    // PublishStateAsync re-reads the device from the DB to build the payload,
    // so it must run after SaveChanges.
    public static async Task SetOwnerAsync(
        SynoraDbContext db, IDeviceMqttPublisher mqtt, Device device, Guid? newOwnerId, string avatarStoragePath)
    {
        if (device.OwnerUserId == newOwnerId)
        {
            return;
        }

        if (device.OwnerUserId is not null)
        {
            await ClearOwnerSettingsAsync(db, device, avatarStoragePath);
        }

        device.OwnerUserId = newOwnerId;
        device.PairedAt = newOwnerId is null ? null : DateTimeOffset.UtcNow;
        await db.SaveChangesAsync();
        await mqtt.PublishStateAsync(device.Id);
    }
}
