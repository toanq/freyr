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
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.DependencyInjection.Extensions;
using Microsoft.IdentityModel.Tokens;
using Synora.Api.Mqtt;
using Synora.Api.Services;
using Synora.Core;
using Synora.Infrastructure;
using Xunit;

namespace Synora.Api.Tests;

// Per-installation push tokens: phone app + web console coexist per user, and
// sends fan out to every token, dropping ones FCM reports as dead.
public class PushTokenTests(WebApplicationFactory<Program> factory) : IClassFixture<WebApplicationFactory<Program>>, IDisposable
{
    private const string JwtFallbackSigningKey = "00000000000000000000000000000000"; // new string('0', 32), see Program.cs
    private readonly List<WebApplicationFactory<Program>> _apps = [];

    public void Dispose()
    {
        foreach (var app in _apps)
        {
            app.Dispose();
        }
    }

    [Fact]
    public async Task PutToken_AppAndWebTokensCoexist_AndReRegisterDoesNotDuplicate()
    {
        var userId = Guid.NewGuid();
        var client = CreateClient(out var dbName, new FakePushSender());
        await SeedUserAsync(dbName, userId);
        client.DefaultRequestHeaders.Authorization = BearerFor(userId, UserRole.User);

        // Phone app's existing body: no platform field.
        Assert.Equal(HttpStatusCode.OK, (await client.PutAsync("/users/me/fcm-token", Json(new { token = "app-token" }))).StatusCode);
        Assert.Equal(HttpStatusCode.OK, (await client.PutAsync("/users/me/fcm-token", Json(new { token = "web-token", platform = "web" }))).StatusCode);
        Assert.Equal(HttpStatusCode.OK, (await client.PutAsync("/users/me/fcm-token", Json(new { token = "web-token", platform = "web" }))).StatusCode);

        await using var db = OpenDb(dbName);
        var tokens = await db.PushTokens.Where(t => t.UserId == userId).OrderBy(t => t.Token).ToListAsync();
        Assert.Equal(2, tokens.Count);
        Assert.Equal(("app-token", "android"), (tokens[0].Token, tokens[0].Platform));
        Assert.Equal(("web-token", "web"), (tokens[1].Token, tokens[1].Platform));
    }

    [Fact]
    public async Task PutToken_UnknownPlatform_ReturnsBadRequest()
    {
        var userId = Guid.NewGuid();
        var client = CreateClient(out var dbName, new FakePushSender());
        await SeedUserAsync(dbName, userId);
        client.DefaultRequestHeaders.Authorization = BearerFor(userId, UserRole.User);

        var response = await client.PutAsync("/users/me/fcm-token", Json(new { token = "t", platform = "windows-phone" }));

        Assert.Equal(HttpStatusCode.BadRequest, response.StatusCode);
    }

    [Fact]
    public async Task DeleteToken_RemovesOnlyCallersMatchingToken()
    {
        var userId = Guid.NewGuid();
        var otherId = Guid.NewGuid();
        var client = CreateClient(out var dbName, new FakePushSender());
        await SeedUserAsync(dbName, userId, ("app-token", "android"), ("web-token", "web"));
        await SeedUserAsync(dbName, otherId, ("other-token", "web"));
        client.DefaultRequestHeaders.Authorization = BearerFor(userId, UserRole.User);

        Assert.Equal(HttpStatusCode.NoContent, (await client.DeleteAsync("/users/me/fcm-token?token=web-token")).StatusCode);
        // Someone else's token is not removable by this user.
        Assert.Equal(HttpStatusCode.NoContent, (await client.DeleteAsync("/users/me/fcm-token?token=other-token")).StatusCode);

        await using var db = OpenDb(dbName);
        Assert.Equal(["app-token", "other-token"], await db.PushTokens.OrderBy(t => t.Token).Select(t => t.Token).ToListAsync());
    }

    [Fact]
    public async Task TestNotification_FansOutToAllTokens_AndRemovesInvalidOnes()
    {
        var userId = Guid.NewGuid();
        var sender = new FakePushSender { InvalidTokens = { "dead-token" } };
        var client = CreateClient(out var dbName, sender);
        await SeedUserAsync(dbName, userId, ("app-token", "android"), ("dead-token", "web"));
        client.DefaultRequestHeaders.Authorization = BearerFor(Guid.NewGuid(), UserRole.SuperAdmin);

        var response = await client.PostAsync($"/users/{userId}/notifications/test", Json(new { title = "Hi", body = "There" }));

        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        Assert.Equal(["app-token", "dead-token"], sender.Sent.Select(s => s.Token).Order().ToList());
        await using var db = OpenDb(dbName);
        Assert.Equal(["app-token"], await db.PushTokens.Select(t => t.Token).ToListAsync());

        using var users = JsonDocument.Parse(await client.GetStringAsync("/users"));
        var user = users.RootElement.EnumerateArray().Single(u => u.GetProperty("id").GetGuid() == userId);
        Assert.True(user.GetProperty("hasPushToken").GetBoolean());
    }

    [Fact]
    public async Task LowWaterAlert_SendsDeviceIdAndDeepLinkToEveryOwnerToken()
    {
        var ownerId = Guid.NewGuid();
        var deviceId = Guid.NewGuid();
        var sender = new FakePushSender();
        var client = CreateClient(out var dbName, sender, new() { ["Domain:App"] = "app.example.test" });
        await SeedUserAsync(dbName, ownerId, ("app-token", "android"), ("web-token", "web"));
        await using (var db = OpenDb(dbName))
        {
            db.Devices.Add(new Device
            {
                Id = deviceId, Name = "Plant 1", ActivationCode = "ABC123", OwnerUserId = ownerId,
                CreatedAt = DateTimeOffset.UtcNow, LowWaterCriticalThresholdPercent = 20,
            });
            await db.SaveChangesAsync();
        }
        client.DefaultRequestHeaders.Authorization = BearerFor(Guid.NewGuid(), UserRole.Admin);

        var response = await client.PostAsync($"/devices/{deviceId}/test-reading", Json(new { waterLevelPercent = 10 }));

        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        Assert.Equal(2, sender.Sent.Count);
        Assert.All(sender.Sent, s =>
        {
            Assert.Equal(deviceId.ToString(), s.Message.Data!["deviceId"]);
            Assert.Equal($"https://app.example.test/?device={deviceId}", s.Message.Link);
        });
    }

    private HttpClient CreateClient(out string dbName, FakePushSender sender, Dictionary<string, string?>? config = null)
    {
        dbName = Guid.NewGuid().ToString();
        var capturedDbName = dbName;
        var app = factory.WithWebHostBuilder(builder =>
        {
            builder.UseEnvironment("Testing");
            if (config is not null)
            {
                builder.ConfigureAppConfiguration(c => c.AddInMemoryCollection(config));
            }
            builder.ConfigureServices(services =>
            {
                services.RemoveAll<IDeviceMqttPublisher>();
                services.AddSingleton<IDeviceMqttPublisher>(new FakeDevicePublisher());
                services.RemoveAll<IPushSender>();
                services.AddSingleton<IPushSender>(sender);
                services.RemoveAll<DbContextOptions<SynoraDbContext>>();
                services.RemoveAll<SynoraDbContext>();
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

    private static async Task SeedUserAsync(string dbName, Guid userId, params (string Token, string Platform)[] tokens)
    {
        await using var db = OpenDb(dbName);
        db.Users.Add(new User { Id = userId, Username = $"u-{userId:N}", Role = UserRole.User, CreatedAt = DateTimeOffset.UtcNow });
        foreach (var (token, platform) in tokens)
        {
            db.PushTokens.Add(new UserPushToken
            {
                UserId = userId, Token = token, Platform = platform,
                CreatedAt = DateTimeOffset.UtcNow, UpdatedAt = DateTimeOffset.UtcNow,
            });
        }
        await db.SaveChangesAsync();
    }

    private static StringContent Json(object body) =>
        new(JsonSerializer.Serialize(body), Encoding.UTF8, "application/json");

    private static readonly InMemoryDatabaseRoot SharedDatabaseRoot = new();

    private static SynoraDbContext OpenDb(string dbName) =>
        new(new DbContextOptionsBuilder<SynoraDbContext>().UseInMemoryDatabase(dbName, SharedDatabaseRoot).Options);

    private static AuthenticationHeaderValue BearerFor(Guid userId, UserRole role)
    {
        var claims = new[]
        {
            new Claim(JwtRegisteredClaimNames.Sub, userId.ToString()),
            new Claim(ClaimTypes.Name, "test-user"),
            new Claim(ClaimTypes.Role, role.ToString()),
        };
        var credentials = new SigningCredentials(
            new SymmetricSecurityKey(Encoding.UTF8.GetBytes(JwtFallbackSigningKey)), SecurityAlgorithms.HmacSha256);
        var token = new JwtSecurityToken(
            issuer: "synora", audience: "synora-mobile", claims: claims,
            expires: DateTime.UtcNow.AddHours(1), signingCredentials: credentials);
        return new AuthenticationHeaderValue("Bearer", new JwtSecurityTokenHandler().WriteToken(token));
    }
}

internal sealed class FakePushSender : IPushSender
{
    public bool IsConfigured => true;
    public HashSet<string> InvalidTokens { get; } = [];
    public List<(string Token, PushMessage Message)> Sent { get; } = [];

    public Task<PushSendOutcome> SendAsync(string token, PushMessage message, CancellationToken ct = default)
    {
        lock (Sent) Sent.Add((token, message));
        return Task.FromResult(InvalidTokens.Contains(token) ? PushSendOutcome.InvalidToken : PushSendOutcome.Sent);
    }
}
