namespace Synora.Core;

public class SensorReading
{
    public Guid Id { get; set; }
    public Guid DeviceId { get; set; }
    public Device? Device { get; set; }
    public double? TemperatureCelsius { get; set; }
    public double? HumidityPercent { get; set; }
    public double? SoilMoisturePercent { get; set; }
    public double? WaterLevelPercent { get; set; }
    public DateTimeOffset RecordedAt { get; set; }
}
