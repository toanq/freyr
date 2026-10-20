using System.Net;
using System.Text;
using System.Text.Json;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Mvc.Testing;
using Microsoft.EntityFrameworkCore;
using Microsoft.EntityFrameworkCore.Storage;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.DependencyInjection.Extensions;
using Synora.Api.Mqtt;
using Synora.Core;
using Synora.Infrastructure;
using Xunit;

namespace Synora.Api.Tests;

// Web-console flavour of the mobile auth endpoints: `?cookie=true` signs the
// browser in with the /auth/login cookie instead of returning a JWT, plus
// the RememberMe flag on /auth/login and the Google web client audience.
public class AuthWebSessionTests(WebApplicationFactory<Program> factory) : IClassFixture<WebApplicationFactory<Program>>, IDisposable
{
    private readonly List<WebApplicationFactory<Program>> _apps = [];

    public void Dispose()
    {
        foreach (var app in _apps)
        {
            app.Dispose();
        }
    }

    [Fact]
    public async Task Register_CookieMode_SignsBrowserInAsUser()
    {
        var client = CreateApp(out _).CreateClient();

        var response = await client.PostAsync("/auth/register?cookie=true",
            Json(new { username = "lan", email = "lan@example.test", password = "secret1" }));

        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        using var session = JsonDocument.Parse(await client.GetStringAsync("/auth/session"));
        Assert.True(session.RootElement.GetProperty("authenticated").GetBoolean());
        Assert.Equal("lan", session.RootElement.GetProperty("username").GetString());
        Assert.Equal("User", session.RootElement.GetProperty("role").GetString());

        using var me = JsonDocument.Parse(await client.GetStringAsync("/users/me"));
        Assert.Equal("lan@example.test", me.RootElement.GetProperty("email").GetString());
    }

    [Fact]
    public async Task Register_TokenMode_WithoutJwtSigningKey_ReturnsServiceUnavailable()
    {
        var client = CreateApp(out _).CreateClient();

        var response = await client.PostAsync("/auth/register", Json(new { username = "lan", password = "secret1" }));

        Assert.Equal(HttpStatusCode.ServiceUnavailable, response.StatusCode);
    }

    [Fact]
    public async Task Register_DuplicateUsername_ReturnsConflict()
    {
        var client = CreateApp(out var dbName).CreateClient();
        await SeedUserAsync(dbName, "lan", "secret1");

        var response = await client.PostAsync("/auth/register?cookie=true", Json(new { username = "lan", password = "other12" }));

        Assert.Equal(HttpStatusCode.Conflict, response.StatusCode);
    }

    [Fact]
    public async Task Register_MissingPassword_ReturnsBadRequest()
    {
        var client = CreateApp(out _).CreateClient();

        var response = await client.PostAsync("/auth/register?cookie=true", Json(new { username = "lan", password = "" }));

        Assert.Equal(HttpStatusCode.BadRequest, response.StatusCode);
    }

    [Theory]
    [InlineData(null, true)]
    [InlineData(true, true)]
    [InlineData(false, false)]
    public async Task Login_RememberMe_ControlsCookiePersistence(bool? rememberMe, bool expectPersistent)
    {
        var app = CreateApp(out var dbName);
        await SeedUserAsync(dbName, "lan", "secret1");
        var client = app.CreateClient(new WebApplicationFactoryClientOptions { HandleCookies = false });

        var response = await client.PostAsync("/auth/login", Json(new { username = "lan", password = "secret1", rememberMe }));

        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        var setCookie = Assert.Single(response.Headers.GetValues("Set-Cookie"), c => c.StartsWith("synora_auth="));
        Assert.Equal(expectPersistent, setCookie.Contains("expires=", StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public async Task Google_CookieMode_NoClientIdsConfigured_ReturnsServiceUnavailable()
    {
        var client = CreateApp(out _).CreateClient();

        var response = await client.PostAsync("/auth/oauth/google?cookie=true", Json(new { idToken = "x.y.z" }));

        Assert.Equal(HttpStatusCode.ServiceUnavailable, response.StatusCode);
    }

    [Fact]
    public async Task Google_WebClientIdOnly_InvalidToken_ReturnsUnauthorized()
    {
        var client = CreateApp(out _, new() { ["OAuth:Google:WebClientId"] = "web-client.apps.googleusercontent.com" })
            .CreateClient();

        var response = await client.PostAsync("/auth/oauth/google?cookie=true", Json(new { idToken = "not-a-jwt" }));

        Assert.Equal(HttpStatusCode.Unauthorized, response.StatusCode);
    }

    [Fact]
    public async Task Facebook_CookieMode_NotConfigured_ReturnsServiceUnavailable()
    {
        var client = CreateApp(out _).CreateClient();

        var response = await client.PostAsync("/auth/oauth/facebook?cookie=true", Json(new { accessToken = "abc" }));

        Assert.Equal(HttpStatusCode.ServiceUnavailable, response.StatusCode);
    }

    private static StringContent Json<T>(T value) =>
        new(JsonSerializer.Serialize(value), Encoding.UTF8, "application/json");

    private static async Task SeedUserAsync(string dbName, string username, string password)
    {
        using var db = OpenDb(dbName);
        var (hash, salt) = PasswordHasher.Hash(password);
        db.Users.Add(new User
        {
            Id = Guid.NewGuid(), Username = username, PasswordHash = hash, PasswordSalt = salt, Role = UserRole.User,
            CreatedAt = DateTimeOffset.UtcNow,
        });
        await db.SaveChangesAsync();
    }

    private WebApplicationFactory<Program> CreateApp(out string dbName, Dictionary<string, string?>? config = null)
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
        return app;
    }

    private static readonly InMemoryDatabaseRoot SharedDatabaseRoot = new();

    private static SynoraDbContext OpenDb(string dbName) =>
        new(new DbContextOptionsBuilder<SynoraDbContext>().UseInMemoryDatabase(dbName, SharedDatabaseRoot).Options);
}
