using Microsoft.EntityFrameworkCore;
using Synora.Core;

namespace Synora.Infrastructure;

public class SynoraDbContext(DbContextOptions<SynoraDbContext> options) : DbContext(options)
{
    public DbSet<User> Users => Set<User>();
    public DbSet<Device> Devices => Set<Device>();
    public DbSet<SensorReading> SensorReadings => Set<SensorReading>();
    public DbSet<WateringSchedule> WateringSchedules => Set<WateringSchedule>();
    public DbSet<DiscussionMessage> DiscussionMessages => Set<DiscussionMessage>();
    public DbSet<SystemSettings> SystemSettings => Set<SystemSettings>();
    public DbSet<MicTestUpload> MicTestUploads => Set<MicTestUpload>();
    public DbSet<FirmwareBuild> FirmwareBuilds => Set<FirmwareBuild>();
    public DbSet<CmsPost> CmsPosts => Set<CmsPost>();
    public DbSet<CmsOrder> CmsOrders => Set<CmsOrder>();

    protected override void OnModelCreating(ModelBuilder modelBuilder)
    {
        modelBuilder.Entity<User>(entity =>
        {
            entity.HasIndex(u => u.Username).IsUnique();
            entity.Property(u => u.Role).HasConversion<string>();
        });

        modelBuilder.Entity<Device>(entity =>
        {
            entity.HasIndex(d => d.ActivationCode).IsUnique();
            entity.HasOne(d => d.Owner)
                  .WithMany()
                  .HasForeignKey(d => d.OwnerUserId)
                  .OnDelete(DeleteBehavior.SetNull);
        });

        modelBuilder.Entity<SensorReading>(entity =>
        {
            entity.HasIndex(r => new { r.DeviceId, r.RecordedAt });
            entity.HasOne(r => r.Device)
                  .WithMany()
                  .HasForeignKey(r => r.DeviceId)
                  .OnDelete(DeleteBehavior.Cascade);
        });

        modelBuilder.Entity<WateringSchedule>(entity =>
        {
            entity.HasIndex(s => s.DeviceId);
            entity.HasOne(s => s.Device)
                  .WithMany()
                  .HasForeignKey(s => s.DeviceId)
                  .OnDelete(DeleteBehavior.Cascade);
        });

        modelBuilder.Entity<DiscussionMessage>(entity =>
        {
            entity.HasIndex(m => new { m.DeviceId, m.CreatedAt });
            entity.HasOne(m => m.Device)
                  .WithMany()
                  .HasForeignKey(m => m.DeviceId)
                  .OnDelete(DeleteBehavior.Cascade);
        });

        modelBuilder.Entity<MicTestUpload>(entity =>
        {
            entity.HasIndex(m => new { m.DeviceId, m.CreatedAt });
            entity.HasOne(m => m.Device)
                  .WithMany()
                  .HasForeignKey(m => m.DeviceId)
                  .OnDelete(DeleteBehavior.Cascade);
        });

        modelBuilder.Entity<FirmwareBuild>(entity =>
        {
            entity.HasIndex(f => f.Version).IsUnique();
        });

        // Landing-site CMS (see CmsPost/CmsOrder); indexes mirror the
        // former Rust CMS's SQLite schema.
        modelBuilder.Entity<CmsPost>(entity =>
        {
            entity.HasIndex(p => p.Slug).IsUnique();
            entity.HasIndex(p => new { p.Published, p.PublishedAt });
            entity.HasIndex(p => p.Category);
        });

        modelBuilder.Entity<CmsOrder>(entity =>
        {
            entity.HasIndex(o => new { o.Status, o.CreatedAt });
        });

        // Seeded once via migration so GET /admin/system-settings can rely
        // on row Id=1 always existing rather than handling a "not created
        // yet" case on every read.
        modelBuilder.Entity<SystemSettings>().HasData(new SystemSettings { Id = 1 });
    }
}
