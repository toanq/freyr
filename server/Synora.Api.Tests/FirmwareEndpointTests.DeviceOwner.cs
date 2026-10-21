using System.Net;
using System.Text;
using System.Text.Json;
using Synora.Core;
using Xunit;

namespace Synora.Api.Tests;

// Admin device-owner endpoints (Devices/AdminDeviceOwnerEndpoints.cs). Lives
// in this partial class to reuse its test host, which swaps in
// FakeDevicePublisher so the isPaired state refresh can be asserted.
public partial class FirmwareEndpointTests
{
    private static StringContent OwnerBody(Guid? userId) =>
        new(JsonSerializer.Serialize(new { userId }), Encoding.UTF8, "application/json");

    private async Task<(Guid DeviceId, Guid OwnerId)> SeedDeviceAsync(string dbName, string? ownerName)
    {
        using var db = OpenDb(dbName);
        Guid ownerId = Guid.Empty;
        if (ownerName is not null)
        {
            ownerId = Guid.NewGuid();
            db.Users.Add(new User { Id = ownerId, Username = ownerName, Role = UserRole.User, CreatedAt = DateTimeOffset.UtcNow });
        }
        var deviceId = Guid.NewGuid();
        db.Devices.Add(new Device
        {
            Id = deviceId, Name = "Plant", ActivationCode = Guid.NewGuid().ToString("N")[..6],
            OwnerUserId = ownerName is null ? null : ownerId,
            PairedAt = ownerName is null ? null : DateTimeOffset.UtcNow.AddDays(-3),
            LowWaterWarningThresholdPercent = 30, LowWaterCriticalThresholdPercent = 10,
            CreatedAt = DateTimeOffset.UtcNow,
        });
        db.WateringSchedules.Add(new WateringSchedule { Id = Guid.NewGuid(), DeviceId = deviceId, Hour = 6, Minute = 0, DurationSeconds = 10 });
        await db.SaveChangesAsync();
        return (deviceId, ownerId);
    }

    private async Task<Guid> SeedUserAsync(string dbName, string username, string? email = null)
    {
        using var db = OpenDb(dbName);
        var id = Guid.NewGuid();
        db.Users.Add(new User { Id = id, Username = username, Email = email, Role = UserRole.User, CreatedAt = DateTimeOffset.UtcNow });
        await db.SaveChangesAsync();
        return id;
    }

    [Fact]
    public async Task DeviceOwner_UserRole_IsForbidden()
    {
        var client = CreateClient(CreateTempDir(), out var dbName);
        var (deviceId, _) = await SeedDeviceAsync(dbName, null);
        client.DefaultRequestHeaders.Authorization = BearerFor(Guid.NewGuid(), UserRole.User);

        Assert.Equal(HttpStatusCode.Forbidden, (await client.GetAsync("/admin/users/lookup?q=al")).StatusCode);
        Assert.Equal(HttpStatusCode.Forbidden, (await client.PutAsync($"/admin/devices/{deviceId}/owner", OwnerBody(null))).StatusCode);
    }

    [Fact]
    public async Task UserLookup_MatchesUsernameOrEmail_CapsAt20_RequiresTwoChars()
    {
        var client = CreateClient(CreateTempDir(), out var dbName);
        await SeedUserAsync(dbName, "Alice", "alice@example.test");
        await SeedUserAsync(dbName, "bob", "bob.alias@example.test");
        await SeedUserAsync(dbName, "carol", "carol@example.test");
        for (var i = 0; i < 25; i++)
        {
            await SeedUserAsync(dbName, $"member{i:00}");
        }
        client.DefaultRequestHeaders.Authorization = BearerFor(Guid.NewGuid(), UserRole.Admin);

        using var ali = JsonDocument.Parse(await client.GetStringAsync("/admin/users/lookup?q=ALI"));
        var names = ali.RootElement.EnumerateArray().Select(u => u.GetProperty("username").GetString()).ToList();
        Assert.Equal(["Alice", "bob"], names);

        using var many = JsonDocument.Parse(await client.GetStringAsync("/admin/users/lookup?q=member"));
        Assert.Equal(20, many.RootElement.GetArrayLength());

        Assert.Equal(HttpStatusCode.BadRequest, (await client.GetAsync("/admin/users/lookup?q=a")).StatusCode);
    }

    [Fact]
    public async Task AssignOwner_ToUnpairedDevice_PairsAndPublishesState()
    {
        var publisher = new FakeDevicePublisher();
        var client = CreateClient(CreateTempDir(), out var dbName, publisher);
        var (deviceId, _) = await SeedDeviceAsync(dbName, null);
        var userId = await SeedUserAsync(dbName, "dana");
        client.DefaultRequestHeaders.Authorization = BearerFor(Guid.NewGuid(), UserRole.Admin);

        var response = await client.PutAsync($"/admin/devices/{deviceId}/owner", OwnerBody(userId));

        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        using var body = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
        Assert.Equal("dana", body.RootElement.GetProperty("owner").GetString());
        using var db = OpenDb(dbName);
        var device = db.Devices.Single(d => d.Id == deviceId);
        Assert.Equal(userId, device.OwnerUserId);
        Assert.NotNull(device.PairedAt);
        Assert.Equal([deviceId], publisher.StateUpdates);
    }

    [Fact]
    public async Task ClearOwner_UnpairsLikeDelete_AndPublishesState()
    {
        var publisher = new FakeDevicePublisher();
        var client = CreateClient(CreateTempDir(), out var dbName, publisher);
        var (deviceId, _) = await SeedDeviceAsync(dbName, "erin");
        client.DefaultRequestHeaders.Authorization = BearerFor(Guid.NewGuid(), UserRole.Admin);

        var response = await client.PutAsync($"/admin/devices/{deviceId}/owner", OwnerBody(null));

        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        using var db = OpenDb(dbName);
        var device = db.Devices.Single(d => d.Id == deviceId);
        Assert.Null(device.OwnerUserId);
        Assert.Null(device.PairedAt);
        Assert.Null(device.LowWaterWarningThresholdPercent);
        Assert.Empty(db.WateringSchedules.Where(s => s.DeviceId == deviceId));
        Assert.Equal([deviceId], publisher.StateUpdates);
    }

    [Fact]
    public async Task ReassignOwner_DropsPreviousOwnersSettings()
    {
        var publisher = new FakeDevicePublisher();
        var client = CreateClient(CreateTempDir(), out var dbName, publisher);
        var (deviceId, oldOwnerId) = await SeedDeviceAsync(dbName, "frank");
        var newOwnerId = await SeedUserAsync(dbName, "gina");
        client.DefaultRequestHeaders.Authorization = BearerFor(Guid.NewGuid(), UserRole.SuperAdmin);

        var response = await client.PutAsync($"/admin/devices/{deviceId}/owner", OwnerBody(newOwnerId));

        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        using var db = OpenDb(dbName);
        var device = db.Devices.Single(d => d.Id == deviceId);
        Assert.Equal(newOwnerId, device.OwnerUserId);
        Assert.NotEqual(oldOwnerId, device.OwnerUserId);
        Assert.True(device.PairedAt > DateTimeOffset.UtcNow.AddMinutes(-1));
        Assert.Null(device.LowWaterCriticalThresholdPercent);
        Assert.Empty(db.WateringSchedules.Where(s => s.DeviceId == deviceId));
        Assert.Equal([deviceId], publisher.StateUpdates);
    }

    [Fact]
    public async Task SetOwner_UnknownDeviceOrUser_ReturnsNotFound()
    {
        var client = CreateClient(CreateTempDir(), out var dbName);
        var (deviceId, _) = await SeedDeviceAsync(dbName, null);
        var userId = await SeedUserAsync(dbName, "hank");
        client.DefaultRequestHeaders.Authorization = BearerFor(Guid.NewGuid(), UserRole.Admin);

        Assert.Equal(HttpStatusCode.NotFound, (await client.PutAsync($"/admin/devices/{Guid.NewGuid()}/owner", OwnerBody(userId))).StatusCode);
        Assert.Equal(HttpStatusCode.NotFound, (await client.PutAsync($"/admin/devices/{deviceId}/owner", OwnerBody(Guid.NewGuid()))).StatusCode);
    }

    [Fact]
    public async Task SetOwner_SameOwner_IsNoOp()
    {
        var publisher = new FakeDevicePublisher();
        var client = CreateClient(CreateTempDir(), out var dbName, publisher);
        var (deviceId, ownerId) = await SeedDeviceAsync(dbName, "ivy");
        client.DefaultRequestHeaders.Authorization = BearerFor(Guid.NewGuid(), UserRole.Admin);

        var response = await client.PutAsync($"/admin/devices/{deviceId}/owner", OwnerBody(ownerId));

        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        using var db = OpenDb(dbName);
        Assert.Single(db.WateringSchedules.Where(s => s.DeviceId == deviceId));
        Assert.Empty(publisher.StateUpdates);
    }
}
