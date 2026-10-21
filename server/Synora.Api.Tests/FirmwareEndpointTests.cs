using System.IdentityModel.Tokens.Jwt;
using System.Net;
using System.Net.Http.Headers;
using System.Security.Claims;
using System.Text;
using System.Text.Json;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Mvc.Testing;
using Microsoft.EntityFrameworkCore;
using Microsoft.EntityFrameworkCore.Storage;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.DependencyInjection.Extensions;
using Microsoft.IdentityModel.Tokens;
using Synora.Api.Mqtt;
using Synora.Core;
using Synora.Infrastructure;
using Xunit;

namespace Synora.Api.Tests;

// Covers the OTA plan's three new endpoints: admin firmware upload/list
// (POST+GET /admin/firmware) and the WiFi-OTA trigger (POST
// /devices/{id}/ota). Unlike HealthEndpointTests, these touch
// SynoraDbContext, and the app always wires that to a real Postgres
// (Program.cs: AddDbContext<SynoraDbContext>(UseNpgsql(...)) -- no
// Testing-environment swap exists), so each test below replaces it with
// EF Core's InMemory provider via WithWebHostBuilder/ConfigureServices and
// points Firmware:StoragePath at a throwaway temp dir, instead of requiring
// a real Postgres instance in the test run.
//
// Auth: these routes take a JWT bearer. Rather than going through
// /auth/token (which 503s unless Jwt:SigningKey is configured), tokens here
// are minted directly with the same issuer/audience/claim shape as
// Program.cs's IssueJwt, signed with the same fallback key
// TokenValidationParameters falls back to when Jwt:SigningKey is unset
// (new string('0', 32)) -- which is always the case under "Testing".
public partial class FirmwareEndpointTests(WebApplicationFactory<Program> factory) : IClassFixture<WebApplicationFactory<Program>>, IDisposable
{
    private const string JwtFallbackSigningKey = "00000000000000000000000000000000"; // new string('0', 32), see Program.cs
    private const string JwtIssuer = "synora";
    private const string JwtAudience = "synora-mobile";

    private readonly List<WebApplicationFactory<Program>> _apps = [];
    private readonly List<string> _tempDirs = [];

    public void Dispose()
    {
        foreach (var app in _apps)
        {
            app.Dispose();
        }
        foreach (var dir in _tempDirs)
        {
            try
            {
                Directory.Delete(dir, recursive: true);
            }
            catch (IOException)
            {
            }
        }
        Environment.SetEnvironmentVariable("Firmware__StoragePath", null);
    }

    [Fact]
    public async Task UploadFirmware_AsAdmin_StoresBuildAndReturnsDto()
    {
        var storageDir = CreateTempDir();
        var client = CreateClient(storageDir, out _);
        client.DefaultRequestHeaders.Authorization = BearerFor(Guid.NewGuid(), UserRole.Admin);

        using var content = new MultipartFormDataContent
        {
            { new StringContent("1.2.3"), "version" },
            { new StringContent("test build"), "notes" },
        };
        var fileBytes = new byte[] { 0xE9, 2, 3, 4, 5 };
        var fileContent = new ByteArrayContent(fileBytes);
        fileContent.Headers.ContentType = new MediaTypeHeaderValue("application/octet-stream");
        content.Add(fileContent, "file", "firmware.bin");

        var response = await client.PostAsync("/admin/firmware", content);

        var raw = await response.Content.ReadAsStringAsync();
        Assert.True(response.StatusCode == HttpStatusCode.OK, $"status={response.StatusCode} body={raw}");
        using var body = JsonDocument.Parse(raw);
        var root = body.RootElement;
        Assert.Equal("1.2.3", root.GetProperty("version").GetString());
        Assert.Equal("test build", root.GetProperty("notes").GetString());
        Assert.Equal(fileBytes.Length, root.GetProperty("sizeBytes").GetInt64());
        Assert.False(string.IsNullOrWhiteSpace(root.GetProperty("sha256").GetString()));

        // Confirms the .bin actually landed under Firmware:StoragePath,
        // named after its own Id (not its version) per the endpoint's doc
        // comment -- not just that the DB row/response looked right.
        var id = root.GetProperty("id").GetGuid();
        Assert.True(File.Exists(Path.Combine(storageDir, $"{id}.bin")));
    }

    [Fact]
    public async Task UploadFirmware_AsNonAdmin_ReturnsForbidden()
    {
        var client = CreateClient(CreateTempDir(), out _);
        client.DefaultRequestHeaders.Authorization = BearerFor(Guid.NewGuid(), UserRole.User);

        using var content = new MultipartFormDataContent { { new StringContent("1.0.0"), "version" } };
        var fileContent = new ByteArrayContent([1, 2, 3]);
        fileContent.Headers.ContentType = new MediaTypeHeaderValue("application/octet-stream");
        content.Add(fileContent, "file", "firmware.bin");

        var response = await client.PostAsync("/admin/firmware", content);

        Assert.Equal(HttpStatusCode.Forbidden, response.StatusCode);
    }

    [Fact]
    public async Task ListFirmware_ReturnsBuildsNewestFirst()
    {
        var client = CreateClient(CreateTempDir(), out var dbName);
        using (var db = OpenDb(dbName))
        {
            db.FirmwareBuilds.AddRange(
                new FirmwareBuild
                {
                    Id = Guid.NewGuid(), Version = "1.0.0", FileName = "a.bin", Sha256 = "aaa",
                    SizeBytes = 10, CreatedAt = DateTimeOffset.UtcNow.AddHours(-1),
                },
                new FirmwareBuild
                {
                    Id = Guid.NewGuid(), Version = "1.1.0", FileName = "b.bin", Sha256 = "bbb",
                    SizeBytes = 20, CreatedAt = DateTimeOffset.UtcNow,
                });
            await db.SaveChangesAsync();
        }
        client.DefaultRequestHeaders.Authorization = BearerFor(Guid.NewGuid(), UserRole.Admin);

        var response = await client.GetAsync("/admin/firmware");

        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        using var body = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
        var builds = body.RootElement.EnumerateArray().ToList();
        Assert.Equal(2, builds.Count);
        Assert.Equal("1.1.0", builds[0].GetProperty("version").GetString());
        Assert.Equal("1.0.0", builds[1].GetProperty("version").GetString());
    }

    [Fact]
    public async Task UploadFirmware_DuplicateVersion_ReturnsConflictWithoutOrphanFile()
    {
        var storageDir = CreateTempDir();
        var client = CreateClient(storageDir, out var dbName);
        using (var db = OpenDb(dbName))
        {
            db.FirmwareBuilds.Add(new FirmwareBuild
            {
                Id = Guid.NewGuid(), Version = "1.2.3", FileName = "existing.bin", Sha256 = "aaa",
                SizeBytes = 10, CreatedAt = DateTimeOffset.UtcNow,
            });
            await db.SaveChangesAsync();
        }
        client.DefaultRequestHeaders.Authorization = BearerFor(Guid.NewGuid(), UserRole.Admin);

        var response = await client.PostAsync("/admin/firmware", FirmwareUpload("1.2.3", [0xE9, 1, 2, 3]));

        Assert.Equal(HttpStatusCode.Conflict, response.StatusCode);
        Assert.Empty(Directory.GetFiles(storageDir));
    }

    [Fact]
    public async Task UploadFirmware_WithoutEspImageMagic_ReturnsBadRequest()
    {
        var storageDir = CreateTempDir();
        var client = CreateClient(storageDir, out _);
        client.DefaultRequestHeaders.Authorization = BearerFor(Guid.NewGuid(), UserRole.Admin);

        var response = await client.PostAsync("/admin/firmware", FirmwareUpload("1.0.0", [0x7F, 1, 2, 3]));

        Assert.Equal(HttpStatusCode.BadRequest, response.StatusCode);
        Assert.Empty(Directory.GetFiles(storageDir));
    }

    [Fact]
    public async Task TriggerOta_WithPublicBaseUrl_PublishesUrlUnderIt()
    {
        var publisher = new FakeDevicePublisher();
        var client = CreateClient(CreateTempDir(), out var dbName, publisher, "https://example.test/api/");
        var (ownerId, deviceId, buildId) = await SeedOwnedDeviceAndBuildAsync(dbName);
        client.DefaultRequestHeaders.Authorization = BearerFor(ownerId, UserRole.User);

        var response = await client.PostAsJsonContent($"/devices/{deviceId}/ota", new { firmwareBuildId = buildId });

        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        var published = Assert.Single(publisher.OtaCommands);
        Assert.Equal(deviceId, published.DeviceId);
        Assert.Equal($"https://example.test/api/firmware/{buildId}/binary", published.DownloadUrl);
    }

    [Fact]
    public async Task TriggerOta_WithAppDomainOnly_PublishesHttpsUrlUnderApi()
    {
        // Domain:App is read before builder.Build(), so it must come from the env var provider.
        Environment.SetEnvironmentVariable("Domain__App", "https://app.example.test/");
        try
        {
            var publisher = new FakeDevicePublisher();
            var client = CreateClient(CreateTempDir(), out var dbName, publisher);
            var (ownerId, deviceId, buildId) = await SeedOwnedDeviceAndBuildAsync(dbName);
            client.DefaultRequestHeaders.Authorization = BearerFor(ownerId, UserRole.User);

            var response = await client.PostAsJsonContent($"/devices/{deviceId}/ota", new { firmwareBuildId = buildId });

            Assert.Equal(HttpStatusCode.OK, response.StatusCode);
            var published = Assert.Single(publisher.OtaCommands);
            Assert.Equal($"https://app.example.test/api/firmware/{buildId}/binary", published.DownloadUrl);
        }
        finally
        {
            Environment.SetEnvironmentVariable("Domain__App", null);
        }
    }

    [Fact]
    public async Task TriggerOta_BrokerDisconnected_ReturnsServiceUnavailable()
    {
        var publisher = new FakeDevicePublisher { IsConnected = false };
        var client = CreateClient(CreateTempDir(), out var dbName, publisher);
        var (ownerId, deviceId, buildId) = await SeedOwnedDeviceAndBuildAsync(dbName);
        client.DefaultRequestHeaders.Authorization = BearerFor(ownerId, UserRole.User);

        var response = await client.PostAsJsonContent($"/devices/{deviceId}/ota", new { firmwareBuildId = buildId });

        Assert.Equal(HttpStatusCode.ServiceUnavailable, response.StatusCode);
        Assert.Empty(publisher.OtaCommands);
    }

    [Fact]
    public async Task TriggerOta_AsDeviceOwner_PublishesCommandAndReturnsOk()
    {
        var client = CreateClient(CreateTempDir(), out var dbName);
        var ownerId = Guid.NewGuid();
        var deviceId = Guid.NewGuid();
        var buildId = Guid.NewGuid();
        using (var db = OpenDb(dbName))
        {
            db.Devices.Add(new Device
            {
                Id = deviceId, Name = "Plant 1", ActivationCode = "ABC123", OwnerUserId = ownerId,
                CreatedAt = DateTimeOffset.UtcNow,
            });
            db.FirmwareBuilds.Add(new FirmwareBuild
            {
                Id = buildId, Version = "2.0.0", FileName = "c.bin", Sha256 = "ccc", SizeBytes = 30,
                CreatedAt = DateTimeOffset.UtcNow,
            });
            await db.SaveChangesAsync();
        }
        client.DefaultRequestHeaders.Authorization = BearerFor(ownerId, UserRole.User);

        var response = await client.PostAsJsonContent($"/devices/{deviceId}/ota", new { firmwareBuildId = buildId });

        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
    }

    [Fact]
    public async Task TriggerOta_UnknownFirmwareBuild_ReturnsBadRequest()
    {
        var client = CreateClient(CreateTempDir(), out var dbName);
        var ownerId = Guid.NewGuid();
        var deviceId = Guid.NewGuid();
        using (var db = OpenDb(dbName))
        {
            db.Devices.Add(new Device
            {
                Id = deviceId, Name = "Plant 1", ActivationCode = "ABC123", OwnerUserId = ownerId,
                CreatedAt = DateTimeOffset.UtcNow,
            });
            await db.SaveChangesAsync();
        }
        client.DefaultRequestHeaders.Authorization = BearerFor(ownerId, UserRole.User);

        var response = await client.PostAsJsonContent($"/devices/{deviceId}/ota", new { firmwareBuildId = Guid.NewGuid() });

        Assert.Equal(HttpStatusCode.BadRequest, response.StatusCode);
    }

    [Fact]
    public async Task TriggerOta_DeviceNotOwnedByCaller_ReturnsNotFound()
    {
        var client = CreateClient(CreateTempDir(), out var dbName);
        var deviceId = Guid.NewGuid();
        var buildId = Guid.NewGuid();
        using (var db = OpenDb(dbName))
        {
            db.Devices.Add(new Device
            {
                Id = deviceId, Name = "Plant 1", ActivationCode = "ABC123", OwnerUserId = Guid.NewGuid(),
                CreatedAt = DateTimeOffset.UtcNow,
            });
            db.FirmwareBuilds.Add(new FirmwareBuild
            {
                Id = buildId, Version = "2.0.0", FileName = "c.bin", Sha256 = "ccc", SizeBytes = 30,
                CreatedAt = DateTimeOffset.UtcNow,
            });
            await db.SaveChangesAsync();
        }
        // A different, unrelated regular user -- not this device's owner, not an admin.
        client.DefaultRequestHeaders.Authorization = BearerFor(Guid.NewGuid(), UserRole.User);

        var response = await client.PostAsJsonContent($"/devices/{deviceId}/ota", new { firmwareBuildId = buildId });

        Assert.Equal(HttpStatusCode.NotFound, response.StatusCode);
    }

    private static MultipartFormDataContent FirmwareUpload(string version, byte[] fileBytes)
    {
        var content = new MultipartFormDataContent { { new StringContent(version), "version" } };
        var fileContent = new ByteArrayContent(fileBytes);
        fileContent.Headers.ContentType = new MediaTypeHeaderValue("application/octet-stream");
        content.Add(fileContent, "file", "firmware.bin");
        return content;
    }

    private static async Task<(Guid OwnerId, Guid DeviceId, Guid BuildId)> SeedOwnedDeviceAndBuildAsync(string dbName)
    {
        var ownerId = Guid.NewGuid();
        var deviceId = Guid.NewGuid();
        var buildId = Guid.NewGuid();
        using var db = OpenDb(dbName);
        db.Devices.Add(new Device
        {
            Id = deviceId, Name = "Plant 1", ActivationCode = "ABC123", OwnerUserId = ownerId,
            CreatedAt = DateTimeOffset.UtcNow,
        });
        db.FirmwareBuilds.Add(new FirmwareBuild
        {
            Id = buildId, Version = "2.0.0", FileName = "c.bin", Sha256 = "ccc", SizeBytes = 30,
            CreatedAt = DateTimeOffset.UtcNow,
        });
        await db.SaveChangesAsync();
        return (ownerId, deviceId, buildId);
    }

    private HttpClient CreateClient(
        string storageDir, out string dbName, FakeDevicePublisher? publisher = null, string? publicBaseUrl = null)
    {
        // The test host has no broker, so default to a connected fake rather than MqttService.
        var fakePublisher = publisher ?? new FakeDevicePublisher();
        dbName = Guid.NewGuid().ToString();
        var capturedDbName = dbName;
        // Program.cs reads Firmware:StoragePath into a plain local variable
        // before builder.Build() runs, and WebApplicationFactory's
        // ConfigureAppConfiguration override is only spliced in around that
        // Build() call -- too late to affect a value already read earlier.
        // The env var provider, by contrast, is already present when
        // WebApplication.CreateBuilder(args) first runs, so it's visible to
        // that earlier read. (xUnit runs this class's tests sequentially --
        // see assembly-level [CollectionBehavior] absence / default
        // collection -- so this process-wide mutation is safe here.)
        Environment.SetEnvironmentVariable("Firmware__StoragePath", storageDir);
        var app = factory.WithWebHostBuilder(builder =>
        {
            builder.UseEnvironment("Testing");
            if (publicBaseUrl is not null)
            {
                builder.ConfigureAppConfiguration(config => config.AddInMemoryCollection(
                    new Dictionary<string, string?> { ["Firmware:PublicBaseUrl"] = publicBaseUrl }));
            }
            builder.ConfigureServices(services =>
            {
                services.RemoveAll<IDeviceMqttPublisher>();
                services.AddSingleton<IDeviceMqttPublisher>(fakePublisher);
                services.RemoveAll<DbContextOptions<SynoraDbContext>>();
                services.RemoveAll<SynoraDbContext>();
                // Isolated per host (not shared/static) -- EF Core validates that
                // logging/warnings config stays constant for a given internal
                // service provider, which breaks when the same static instance is
                // reused across hosts with different ILoggerFactory instances.
                var efServiceProvider = new ServiceCollection().AddEntityFrameworkInMemoryDatabase().BuildServiceProvider();
                services.AddDbContext<SynoraDbContext>(options =>
                {
                    options.UseInMemoryDatabase(capturedDbName, SharedDatabaseRoot);
                    options.UseInternalServiceProvider(efServiceProvider);
                });
            });
        });
        _apps.Add(app);
        return app.CreateClient();
    }

    // Shared by CreateClient and OpenDb so both see the same backing store
    // for a given dbName, even though each resolves the InMemory provider
    // from a different internal service provider -- InMemoryDatabaseRoot is
    // the documented way to share data across those, independent of which
    // service provider instance is querying it.
    private static readonly InMemoryDatabaseRoot SharedDatabaseRoot = new();

    private static SynoraDbContext OpenDb(string dbName)
    {
        var options = new DbContextOptionsBuilder<SynoraDbContext>()
            .UseInMemoryDatabase(dbName, SharedDatabaseRoot)
            .Options;
        return new SynoraDbContext(options);
    }

    private string CreateTempDir()
    {
        var dir = Path.Combine(Path.GetTempPath(), "synora-firmware-tests-" + Guid.NewGuid());
        Directory.CreateDirectory(dir);
        _tempDirs.Add(dir);
        return dir;
    }

    private static AuthenticationHeaderValue BearerFor(Guid userId, UserRole role)
    {
        var claims = new[]
        {
            new Claim(JwtRegisteredClaimNames.Sub, userId.ToString()),
            new Claim(ClaimTypes.Name, "test-user"),
            new Claim(ClaimTypes.Role, role.ToString()),
        };
        var credentials = new SigningCredentials(
            new SymmetricSecurityKey(Encoding.UTF8.GetBytes(JwtFallbackSigningKey)),
            SecurityAlgorithms.HmacSha256);
        var token = new JwtSecurityToken(
            issuer: JwtIssuer, audience: JwtAudience, claims: claims,
            expires: DateTime.UtcNow.AddHours(1), signingCredentials: credentials);
        return new AuthenticationHeaderValue("Bearer", new JwtSecurityTokenHandler().WriteToken(token));
    }
}

internal sealed class FakeDevicePublisher : IDeviceMqttPublisher
{
    public bool IsConnected { get; set; } = true;
    public List<(Guid DeviceId, string Version, string DownloadUrl)> OtaCommands { get; } = [];
    public List<Guid> StateUpdates { get; } = [];

    public Task PublishStateAsync(Guid deviceId, CancellationToken ct = default)
    {
        StateUpdates.Add(deviceId);
        return Task.CompletedTask;
    }
    public Task PublishWaterCommandAsync(Guid deviceId, int durationSeconds, CancellationToken ct = default) => Task.CompletedTask;
    public Task PublishPingCommandAsync(Guid deviceId, CancellationToken ct = default) => Task.CompletedTask;

    public Task PublishOtaCommandAsync(Guid deviceId, string version, string downloadUrl, CancellationToken ct = default)
    {
        OtaCommands.Add((deviceId, version, downloadUrl));
        return Task.CompletedTask;
    }
}

internal static class HttpClientJsonExtensions
{
    // Program.cs's ConfigureHttpJsonOptions only adds a JsonStringEnumConverter
    // and otherwise relies on System.Text.Json's default camelCase policy --
    // PostAsJsonAsync from System.Net.Http.Json matches that out of the box,
    // but is wrapped here so every call site below stays a one-liner.
    public static Task<HttpResponseMessage> PostAsJsonContent<T>(this HttpClient client, string url, T value) =>
        client.PostAsync(url, new StringContent(JsonSerializer.Serialize(value), Encoding.UTF8, "application/json"));
}
