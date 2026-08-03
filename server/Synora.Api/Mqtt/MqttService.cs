using System.Buffers;
using System.Text.Json;
using Microsoft.EntityFrameworkCore;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using MQTTnet;
using MQTTnet.Protocol;
using Synora.Api.Services;
using Synora.Core;
using Synora.Infrastructure;
using Synora.Voice;

namespace Synora.Api.Mqtt;

/// <summary>
/// The server's single long-lived MQTT client to Mosquitto. Subscribes to the
/// device-trust-model wildcard topics (telemetry/unlink) and the voice
/// control/audio "up" wildcard topics, and is the one place that knows how to
/// publish back (device commands/state via <see cref="IDeviceMqttPublisher"/>,
/// voice control/audio "down" via <see cref="IVoicePublisher"/>).
///
/// This service depends on <see cref="VoiceMqttDispatcher"/> to route
/// incoming voice topics to the pipeline, and <see cref="VoiceMqttDispatcher"/>
/// needs an <see cref="IVoicePublisher"/> (this class) to construct outgoing
/// <see cref="VoiceSession"/>s -- which would be a circular DI dependency if
/// wired up via constructor injection both ways. Broken by NOT
/// constructor-injecting <see cref="IVoicePublisher"/> into
/// <see cref="VoiceMqttDispatcher"/>: instead, <see cref="OnMessageReceivedAsync"/>
/// passes <c>this</c> straight into <see cref="VoiceMqttDispatcher.HandleControlAsync"/>
/// as a plain method argument, which the DI container's cycle detection
/// doesn't see at all.
///
/// Business logic for telemetry/unlink is the same as the old REST endpoints
/// (see git history of Program.cs) -- only the transport changed. Low-water
/// alerting is shared via <see cref="LowWaterAlertService"/> so it can't
/// drift from the admin-only test-reading endpoint that still uses it.
/// </summary>
public sealed class MqttService : BackgroundService, IDeviceMqttPublisher, IVoicePublisher
{
    private const string TopicPrefix = "synora/devices/";
    private static readonly TimeSpan ReconnectDelay = TimeSpan.FromSeconds(5);
    // Firmware publishes camelCase telemetry keys (e.g. "soilMoisturePercent");
    // System.Text.Json's default Deserialize() is case-sensitive, so without
    // this the PascalCase TelemetryPayload properties silently stay null.
    private static readonly JsonSerializerOptions TelemetryJsonOptions = new() { PropertyNameCaseInsensitive = true };

    private readonly MqttOptions _options;
    private readonly IServiceScopeFactory _scopeFactory;
    private readonly VoiceMqttDispatcher _voiceDispatcher;
    private readonly LowWaterAlertService _lowWaterAlerts;
    private readonly ILogger<MqttService> _logger;
    private readonly MqttClientFactory _factory = new();
    private IMqttClient? _client;

    public bool IsConnected => _client is { IsConnected: true };

    public MqttService(
        IOptions<MqttOptions> options,
        IServiceScopeFactory scopeFactory,
        VoiceMqttDispatcher voiceDispatcher,
        LowWaterAlertService lowWaterAlerts,
        ILogger<MqttService> logger)
    {
        _options = options.Value;
        _scopeFactory = scopeFactory;
        _voiceDispatcher = voiceDispatcher;
        _lowWaterAlerts = lowWaterAlerts;
        _logger = logger;
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        _client = _factory.CreateMqttClient();
        _client.ApplicationMessageReceivedAsync += OnMessageReceivedAsync;

        var clientOptions = new MqttClientOptionsBuilder()
            .WithTcpServer(_options.Host, _options.Port)
            .WithCredentials(_options.Username, _options.Password)
            .WithClientId($"synora-server-{Guid.NewGuid():N}")
            .WithCleanSession()
            .Build();

        // Plain (non-managed) client + a manual reconnect-poll loop -- simple
        // and sufficient for this dev/bench setup; MQTTnet's managed client
        // (auto-reconnect + offline publish queueing) is a reasonable future
        // upgrade if reconnect churn becomes a real problem.
        while (!stoppingToken.IsCancellationRequested)
        {
            try
            {
                if (!_client.IsConnected)
                {
                    await _client.ConnectAsync(clientOptions, stoppingToken);
                    await SubscribeAsync(stoppingToken);
                    _logger.LogInformation("Connected to MQTT broker at {Host}:{Port}", _options.Host, _options.Port);
                }
            }
            catch (Exception ex) when (ex is not OperationCanceledException)
            {
                _logger.LogWarning(ex, "MQTT connect to {Host}:{Port} failed, retrying in {Delay}s",
                    _options.Host, _options.Port, ReconnectDelay.TotalSeconds);
            }

            try
            {
                await Task.Delay(ReconnectDelay, stoppingToken);
            }
            catch (OperationCanceledException)
            {
                // shutting down
            }
        }
    }

    public override async Task StopAsync(CancellationToken cancellationToken)
    {
        if (_client is { IsConnected: true })
        {
            try
            {
                await _client.DisconnectAsync(cancellationToken: cancellationToken);
            }
            catch (Exception ex)
            {
                _logger.LogWarning(ex, "Error disconnecting MQTT client during shutdown");
            }
        }
        await base.StopAsync(cancellationToken);
    }

    private Task SubscribeAsync(CancellationToken ct) =>
        _client!.SubscribeAsync(new MqttClientSubscribeOptionsBuilder()
            .WithTopicFilter(f => f.WithTopic($"{TopicPrefix}+/telemetry").WithAtLeastOnceQoS())
            .WithTopicFilter(f => f.WithTopic($"{TopicPrefix}+/unlink").WithAtLeastOnceQoS())
            .WithTopicFilter(f => f.WithTopic($"{TopicPrefix}+/voice/control/up").WithAtLeastOnceQoS())
            .WithTopicFilter(f => f.WithTopic($"{TopicPrefix}+/voice/audio/up").WithAtMostOnceQoS())
            .Build(), ct);

    private async Task OnMessageReceivedAsync(MqttApplicationMessageReceivedEventArgs e)
    {
        var topic = e.ApplicationMessage.Topic;
        if (!TryParseTopic(topic, out var deviceId, out var suffix))
        {
            return;
        }

        var payload = e.ApplicationMessage.Payload.ToArray();
        try
        {
            switch (suffix)
            {
                case "telemetry":
                    await HandleTelemetryAsync(deviceId, payload);
                    break;
                case "unlink":
                    await HandleUnlinkAsync(deviceId);
                    break;
                case "voice/control/up":
                    await _voiceDispatcher.HandleControlAsync(deviceId, payload, this, CancellationToken.None);
                    break;
                case "voice/audio/up":
                    await _voiceDispatcher.HandleAudioAsync(deviceId, payload, CancellationToken.None);
                    break;
            }
        }
        catch (Exception ex)
        {
            _logger.LogError(ex, "Error handling MQTT message on topic {Topic}", topic);
        }
    }

    // synora/devices/{deviceId}/{suffix...} -> (deviceId, suffix), e.g.
    // suffix = "telemetry" or "voice/control/up".
    private static bool TryParseTopic(string topic, out Guid deviceId, out string suffix)
    {
        deviceId = Guid.Empty;
        suffix = "";
        var parts = topic.Split('/');
        if (parts.Length < 4 || parts[0] != "synora" || parts[1] != "devices")
        {
            return false;
        }
        if (!Guid.TryParse(parts[2], out deviceId))
        {
            return false;
        }
        suffix = string.Join('/', parts[3..]);
        return true;
    }

    // --- Device-trust-model handlers (mirror the removed REST endpoints exactly) ---

    private async Task HandleTelemetryAsync(Guid deviceId, byte[] payload)
    {
        TelemetryPayload? request;
        try
        {
            request = JsonSerializer.Deserialize<TelemetryPayload>(payload, TelemetryJsonOptions);
        }
        catch (JsonException ex)
        {
            _logger.LogWarning(ex, "Malformed telemetry payload for device {DeviceId}", deviceId);
            return;
        }
        if (request is null)
        {
            return;
        }

        using var scope = _scopeFactory.CreateScope();
        var db = scope.ServiceProvider.GetRequiredService<SynoraDbContext>();

        var device = await db.Devices.Include(d => d.Owner).FirstOrDefaultAsync(d => d.Id == deviceId);
        if (device is null)
        {
            _logger.LogWarning("Telemetry published for unknown device {DeviceId}", deviceId);
            return;
        }

        db.SensorReadings.Add(new SensorReading
        {
            DeviceId = deviceId,
            TemperatureCelsius = request.TemperatureCelsius,
            HumidityPercent = request.HumidityPercent,
            SoilMoisturePercent = request.SoilMoisturePercent,
            WaterLevelPercent = request.WaterLevelPercent,
            RecordedAt = DateTimeOffset.UtcNow,
        });

        device.LastSeenAt = DateTimeOffset.UtcNow;
        device.XiaozhiActivationCode = string.IsNullOrEmpty(request.XiaozhiActivationCode)
            ? null
            : request.XiaozhiActivationCode;
        await _lowWaterAlerts.CheckLowWaterAlertAsync(device, request.WaterLevelPercent);
        await db.SaveChangesAsync();
    }

    private async Task HandleUnlinkAsync(Guid deviceId)
    {
        using var scope = _scopeFactory.CreateScope();
        var db = scope.ServiceProvider.GetRequiredService<SynoraDbContext>();

        var device = await db.Devices.FirstOrDefaultAsync(d => d.Id == deviceId);
        if (device is null)
        {
            return;
        }

        device.OwnerUserId = null;
        device.PairedAt = null;
        await db.SaveChangesAsync();

        await PublishStateCoreAsync(deviceId, db, CancellationToken.None);
    }

    // --- IDeviceMqttPublisher (called from Program.cs's authenticated endpoints) ---

    public async Task PublishStateAsync(Guid deviceId, CancellationToken ct = default)
    {
        using var scope = _scopeFactory.CreateScope();
        var db = scope.ServiceProvider.GetRequiredService<SynoraDbContext>();
        await PublishStateCoreAsync(deviceId, db, ct);
    }

    private async Task PublishStateCoreAsync(Guid deviceId, SynoraDbContext db, CancellationToken ct)
    {
        var device = await db.Devices.AsNoTracking().FirstOrDefaultAsync(d => d.Id == deviceId, ct);
        if (device is null)
        {
            return;
        }

        var schedules = await db.WateringSchedules.AsNoTracking()
            .Where(s => s.DeviceId == deviceId)
            .OrderBy(s => s.Hour).ThenBy(s => s.Minute)
            .Select(s => new { s.Hour, s.Minute, s.DurationSeconds, s.Enabled })
            .ToListAsync(ct);

        var payload = JsonSerializer.Serialize(new
        {
            muted = device.Muted,
            isPaired = device.OwnerUserId != null,
            schedules = schedules.Select(s => new
            {
                hour = s.Hour,
                minute = s.Minute,
                durationSeconds = s.DurationSeconds,
                enabled = s.Enabled,
            }),
            // The device's only time source (no public NTP, no RTC yet) --
            // it applies a hardcoded Vietnam (UTC+7) offset on top of this
            // UTC value itself; see firmware's WateringScheduler::ApplyServerTime.
            serverTimeUnixMs = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(),
        });

        await PublishAsync($"{TopicPrefix}{deviceId}/state", payload, MqttQualityOfServiceLevel.AtLeastOnce, retain: true, ct);
    }

    public Task PublishWaterCommandAsync(Guid deviceId, int durationSeconds, CancellationToken ct = default) =>
        PublishAsync(
            $"{TopicPrefix}{deviceId}/commands/water",
            JsonSerializer.Serialize(new { durationSeconds }),
            MqttQualityOfServiceLevel.AtLeastOnce,
            retain: false,
            ct);

    public Task PublishPingCommandAsync(Guid deviceId, CancellationToken ct = default) =>
        PublishAsync(
            $"{TopicPrefix}{deviceId}/commands/ping",
            "{}",
            MqttQualityOfServiceLevel.AtLeastOnce,
            retain: false,
            ct);

    // --- IVoicePublisher (VoiceSession's send side, see the class doc comment
    // above for why this is a direct interface implementation rather than a
    // separate class depending on this one) ---

    public Task PublishControlAsync(Guid deviceId, string json, CancellationToken ct) =>
        PublishAsync($"{TopicPrefix}{deviceId}/voice/control/down", json, MqttQualityOfServiceLevel.AtLeastOnce, retain: false, ct);

    public Task PublishAudioAsync(Guid deviceId, ReadOnlyMemory<byte> opus, CancellationToken ct) =>
        PublishBytesAsync($"{TopicPrefix}{deviceId}/voice/audio/down", opus, MqttQualityOfServiceLevel.AtMostOnce, ct);

    // --- Generic publish, reused by IDeviceMqttPublisher/IVoicePublisher above. ---

    private async Task PublishAsync(string topic, string payload, MqttQualityOfServiceLevel qos, bool retain, CancellationToken ct)
    {
        if (_client is not { IsConnected: true })
        {
            _logger.LogWarning("Dropped MQTT publish to {Topic} -- not connected to broker", topic);
            return;
        }

        var message = new MqttApplicationMessageBuilder()
            .WithTopic(topic)
            .WithPayload(payload)
            .WithQualityOfServiceLevel(qos)
            .WithRetainFlag(retain)
            .Build();
        await _client.PublishAsync(message, ct);
    }

    private async Task PublishBytesAsync(string topic, ReadOnlyMemory<byte> payload, MqttQualityOfServiceLevel qos, CancellationToken ct)
    {
        if (_client is not { IsConnected: true })
        {
            _logger.LogWarning("Dropped MQTT publish to {Topic} -- not connected to broker", topic);
            return;
        }

        var message = new MqttApplicationMessageBuilder()
            .WithTopic(topic)
            .WithPayload(payload.ToArray())
            .WithQualityOfServiceLevel(qos)
            .Build();
        await _client.PublishAsync(message, ct);
    }

    // Mirrors the old TelemetryRequest DTO shape exactly (see Program.cs) --
    // kept separate rather than shared, since the REST DTO stays in Program.cs
    // for the still-REST admin test-reading endpoint.
    private sealed record TelemetryPayload(
        double? TemperatureCelsius,
        double? HumidityPercent,
        double? SoilMoisturePercent,
        double? WaterLevelPercent,
        string? XiaozhiActivationCode = null);
}
