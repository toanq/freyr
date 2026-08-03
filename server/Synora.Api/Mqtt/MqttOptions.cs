namespace Synora.Api.Mqtt;

/// <summary>
/// Connection settings for the server's single long-lived MQTT client to
/// Mosquitto. Bound from the "Mqtt" config section (appsettings.json +
/// Mqtt__* env var overrides), mirroring the "Llm" section/Llm__* pattern.
/// </summary>
public sealed class MqttOptions
{
    public string Host { get; set; } = "mosquitto";
    public int Port { get; set; } = 1883;
    public string Username { get; set; } = "";
    public string Password { get; set; } = "";
}
