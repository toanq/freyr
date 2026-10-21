using Microsoft.EntityFrameworkCore;
using Synora.Core;
using Synora.Infrastructure;

namespace Synora.Api.Devices;

// Shared projection behind GET /devices, GET /devices/{id} and the admin
// owner endpoint, so every caller gets the exact same DeviceSummaryDto shape.
internal static class DeviceSummaries
{
    public static async Task<List<DeviceSummaryDto>> ListAsync(SynoraDbContext db, IQueryable<Device> query)
    {
        var rows = await query
            .Select(d => new
            {
                d.Id,
                d.Name,
                d.MacAddress,
                d.PairedAt,
                d.LastSeenAt,
                d.LowWaterWarningThresholdPercent,
                d.LowWaterCriticalThresholdPercent,
                d.LowWaterAlertIntervalMinutes,
                d.XiaozhiActivationCode,
                d.Muted,
                d.FirmwareVersion,
                AvatarUrl = d.AvatarPath != null ? $"/devices/{d.Id}/avatar" : null,
                Owner = d.Owner != null ? d.Owner.Username : null,
                LatestReading = db.SensorReadings
                    .Where(r => r.DeviceId == d.Id)
                    .OrderByDescending(r => r.RecordedAt)
                    .Select(r => new SensorReadingDto(
                        r.TemperatureCelsius, r.HumidityPercent, r.SoilMoisturePercent, r.WaterLevelPercent, r.RecordedAt))
                    .FirstOrDefault(),
            })
            .ToListAsync();

        // Computed in memory (not translatable as a clean single SQL projection)
        // -- was duplicated client-side logic in the Flutter app before, moved
        // here so both the app and any future client get the same suggestion
        // from one place. Projected into a named record (not an anonymous type)
        // -- Swashbuckle can't produce a valid $ref name for an anonymous type
        // wrapping another anonymous type (LatestReading), which broke Swagger
        // UI's rendering entirely ("Unable to render this definition").
        return rows.Select(d => new DeviceSummaryDto(
            d.Id,
            d.Name,
            d.MacAddress,
            d.PairedAt,
            d.LowWaterWarningThresholdPercent,
            d.LowWaterCriticalThresholdPercent,
            d.LowWaterAlertIntervalMinutes,
            d.AvatarUrl,
            d.Owner,
            d.LatestReading,
            CareSuggestion(
                d.LatestReading?.SoilMoisturePercent,
                d.LatestReading?.TemperatureCelsius,
                d.LatestReading?.HumidityPercent,
                d.LatestReading?.WaterLevelPercent),
            IsOnline(d.LastSeenAt),
            d.XiaozhiActivationCode,
            d.Muted,
            d.FirmwareVersion)).ToList();
    }

    // Any device-trust-model request (telemetry/pending-command/schedules) counts
    // as a heartbeat -- IsOnline is computed from LastSeenAt at query time (not
    // persisted) so this threshold can be tuned without a migration.
    public static bool IsOnline(DateTimeOffset? lastSeenAt) =>
        lastSeenAt is not null && lastSeenAt > DateTimeOffset.UtcNow.AddSeconds(-60);

    // Mirrors the thresholds/copy the Flutter app used to compute client-side
    // (device_detail_screen.dart's _statusFor/"AI care suggestion" card) --
    // same four metrics, same low/high bands, same three messages.
    public static string CareSuggestion(double? soil, double? temperature, double? humidity, double? water)
    {
        static string StatusFor(double? value, double low, double high) =>
            value is null ? "—" : value < low ? "Thấp" : value > high ? "Cao" : "Tốt";

        var soilStatus = StatusFor(soil, 30, 60);
        var tempStatus = StatusFor(temperature, 18, 30);
        var humidityStatus = StatusFor(humidity, 40, 80);
        var waterStatus = StatusFor(water, 20, 100);

        if (soilStatus == "Thấp")
        {
            return "Đất khá khô, bạn nên tưới nước cho cây.";
        }
        if (new[] { soilStatus, tempStatus, humidityStatus, waterStatus }.All(s => s == "Tốt"))
        {
            return "Cây của bạn đang phát triển tốt, chưa cần chăm sóc thêm.";
        }
        return "Một vài chỉ số chưa lý tưởng, hãy theo dõi cây trong vài ngày tới.";
    }
}
