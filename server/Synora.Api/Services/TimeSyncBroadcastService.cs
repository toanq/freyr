using Microsoft.EntityFrameworkCore;
using Synora.Api.Mqtt;
using Synora.Infrastructure;

namespace Synora.Api.Services;

/// <summary>
/// Re-publishes every paired device's retained MQTT state (see
/// MqttService.PublishStateCoreAsync) once a minute, purely so
/// `serverTimeUnixMs` stays fresh -- a device only ever receives that field
/// via this retained topic, and it was otherwise only republished on
/// explicit owner/admin actions (mute toggle, schedule CRUD, pair/unlink).
/// Between those, a device that reconnects (WiFi/MQTT flapping -- this
/// bench's network has shown plenty of that) would re-receive whatever
/// stale timestamp was last published and reset its clock backward to it,
/// which looks exactly like "the clock is stuck" rather than "drifting" --
/// this closes that gap without needing anything firmware-side to change.
/// </summary>
public sealed class TimeSyncBroadcastService(
    IServiceScopeFactory scopeFactory, IDeviceMqttPublisher publisher, ILogger<TimeSyncBroadcastService> logger)
    : BackgroundService
{
    private static readonly TimeSpan Interval = TimeSpan.FromMinutes(1);

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        using var timer = new PeriodicTimer(Interval);
        while (await timer.WaitForNextTickAsync(stoppingToken))
        {
            try
            {
                using var scope = scopeFactory.CreateScope();
                var db = scope.ServiceProvider.GetRequiredService<SynoraDbContext>();
                var deviceIds = await db.Devices.AsNoTracking()
                    .Where(d => d.OwnerUserId != null)
                    .Select(d => d.Id)
                    .ToListAsync(stoppingToken);

                foreach (var deviceId in deviceIds)
                {
                    await publisher.PublishStateAsync(deviceId, stoppingToken);
                }
            }
            catch (Exception ex) when (ex is not OperationCanceledException)
            {
                logger.LogWarning(ex, "Periodic time-sync broadcast failed");
            }
        }
    }
}
