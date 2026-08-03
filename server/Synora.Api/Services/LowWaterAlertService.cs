using FirebaseAdmin.Messaging;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.Logging;
using Synora.Core;

namespace Synora.Api.Services;

/// <summary>
/// Low-water push-alert logic, extracted so it can be shared between the
/// admin-only <c>POST /devices/{id}/test-reading</c> endpoint and the MQTT
/// telemetry handler (<c>synora/devices/{id}/telemetry</c>) -- both need to
/// run exactly the same check so the alert-firing logic can't drift between
/// the two paths. Only <see cref="Device.LowWaterCriticalThresholdPercent"/>
/// sends a push (LowWaterWarningThresholdPercent is in-app-only, checked
/// client-side against the latest reading).
/// </summary>
public sealed class LowWaterAlertService(IConfiguration configuration, ILogger<LowWaterAlertService> logger)
{
    private readonly bool _firebaseConfigured = !string.IsNullOrEmpty(configuration["Firebase:ServiceAccountJson"]);

    public async Task CheckLowWaterAlertAsync(Device device, double? waterLevelPercent)
    {
        if (device.LowWaterCriticalThresholdPercent is not int critical || waterLevelPercent is not double level)
        {
            return;
        }

        if (level <= critical)
        {
            // LowWaterLastAlertedAt is null the moment a "below threshold" streak
            // starts, so the first check always fires. After that, IntervalMinutes
            // == null means alert once per streak (never fires again until it
            // recovers and drops again); otherwise it re-fires once
            // IntervalMinutes has elapsed since the last alert.
            var dueForAlert = device.LowWaterLastAlertedAt is null ||
                (device.LowWaterAlertIntervalMinutes is int minutes &&
                 DateTimeOffset.UtcNow - device.LowWaterLastAlertedAt >= TimeSpan.FromMinutes(minutes));

            if (dueForAlert)
            {
                device.LowWaterLastAlertedAt = DateTimeOffset.UtcNow;
                if (_firebaseConfigured && !string.IsNullOrEmpty(device.Owner?.FcmToken))
                {
                    try
                    {
                        await FirebaseMessaging.DefaultInstance.SendAsync(new Message
                        {
                            Token = device.Owner!.FcmToken,
                            Notification = new FirebaseAdmin.Messaging.Notification
                            {
                                Title = "Mực nước thấp",
                                Body = $"{device.Name} có mực nước còn {level:0}% (ngưỡng {critical}%).",
                            },
                        });
                    }
                    catch (FirebaseMessagingException ex)
                    {
                        logger.LogWarning(ex, "Failed to send low-water alert for device {DeviceId}", device.Id);
                    }
                }
            }
        }
        else
        {
            device.LowWaterLastAlertedAt = null;
        }
    }
}
