using System.Net;
using System.Text.Json;
using Microsoft.EntityFrameworkCore;
using Synora.Core;
using Xunit;

namespace Synora.Api.Tests;

// Firmware manager (web console) endpoints: build deletion and the
// firmwareVersion field the device list exposes. Shares helpers with
// FirmwareEndpointTests.cs.
public partial class FirmwareEndpointTests
{
    [Fact]
    public async Task DeleteFirmware_AsAdmin_RemovesRowAndFile()
    {
        var storageDir = CreateTempDir();
        var client = CreateClient(storageDir, out var dbName);
        var (_, _, buildId) = await SeedOwnedDeviceAndBuildAsync(dbName);
        var filePath = Path.Combine(storageDir, "c.bin");
        await File.WriteAllBytesAsync(filePath, [0xE9, 1]);
        client.DefaultRequestHeaders.Authorization = BearerFor(Guid.NewGuid(), UserRole.Admin);

        var response = await client.DeleteAsync($"/admin/firmware/{buildId}");

        Assert.Equal(HttpStatusCode.NoContent, response.StatusCode);
        Assert.False(File.Exists(filePath));
        using var db = OpenDb(dbName);
        Assert.False(await db.FirmwareBuilds.AnyAsync(f => f.Id == buildId));
    }

    [Fact]
    public async Task DeleteFirmware_AsUser_ReturnsForbidden()
    {
        var client = CreateClient(CreateTempDir(), out var dbName);
        var (_, _, buildId) = await SeedOwnedDeviceAndBuildAsync(dbName);
        client.DefaultRequestHeaders.Authorization = BearerFor(Guid.NewGuid(), UserRole.User);

        var response = await client.DeleteAsync($"/admin/firmware/{buildId}");

        Assert.Equal(HttpStatusCode.Forbidden, response.StatusCode);
        using var db = OpenDb(dbName);
        Assert.True(await db.FirmwareBuilds.AnyAsync(f => f.Id == buildId));
    }

    [Fact]
    public async Task DeleteFirmware_Unknown_ReturnsNotFound()
    {
        var client = CreateClient(CreateTempDir(), out _);
        client.DefaultRequestHeaders.Authorization = BearerFor(Guid.NewGuid(), UserRole.Admin);

        var response = await client.DeleteAsync($"/admin/firmware/{Guid.NewGuid()}");

        Assert.Equal(HttpStatusCode.NotFound, response.StatusCode);
    }

    [Fact]
    public async Task ListDevices_IncludesFirmwareVersion()
    {
        var client = CreateClient(CreateTempDir(), out var dbName);
        var (_, deviceId, _) = await SeedOwnedDeviceAndBuildAsync(dbName);
        using (var db = OpenDb(dbName))
        {
            var row = await db.Devices.FirstAsync(d => d.Id == deviceId);
            row.FirmwareVersion = "1.4.2";
            await db.SaveChangesAsync();
        }
        client.DefaultRequestHeaders.Authorization = BearerFor(Guid.NewGuid(), UserRole.Admin);

        var raw = await client.GetStringAsync("/devices");

        using var body = JsonDocument.Parse(raw);
        var device = Assert.Single(body.RootElement.EnumerateArray());
        Assert.Equal("1.4.2", device.GetProperty("firmwareVersion").GetString());
    }
}
