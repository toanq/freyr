namespace Synora.Core;

public class WateringSchedule
{
    public Guid Id { get; set; }
    public Guid DeviceId { get; set; }
    public Device? Device { get; set; }
    public int Hour { get; set; }
    public int Minute { get; set; }
    public int DurationSeconds { get; set; }
    public bool Enabled { get; set; } = true;
}
