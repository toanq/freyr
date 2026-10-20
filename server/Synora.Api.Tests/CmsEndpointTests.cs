using System.IdentityModel.Tokens.Jwt;
using System.Net;
using System.Net.Http.Headers;
using System.Security.Claims;
using System.Text;
using System.Text.Json;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Mvc.Testing;
using Microsoft.Data.Sqlite;
using Microsoft.EntityFrameworkCore;
using Microsoft.EntityFrameworkCore.Storage;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.DependencyInjection.Extensions;
using Microsoft.Extensions.Logging.Abstractions;
using Microsoft.IdentityModel.Tokens;
using Synora.Api.Cms;
using Synora.Api.Mqtt;
using Synora.Core;
using Synora.Infrastructure;
using Xunit;

namespace Synora.Api.Tests;

// Landing-site CMS (/cms/*, Cms/CmsEndpoints.cs) and its one-time legacy
// SQLite import (Cms/CmsLegacyImporter.cs). Same host setup as
// FirmwareEndpointTests: InMemory EF behind a shared InMemoryDatabaseRoot,
// JWTs minted with the fallback signing key. Cms:UploadsPath is read after
// Build() (in MapCms), so it's injected via ConfigureAppConfiguration rather
// than a process-wide env var.
public class CmsEndpointTests(WebApplicationFactory<Program> factory) : IClassFixture<WebApplicationFactory<Program>>, IDisposable
{
    private const string JwtFallbackSigningKey = "00000000000000000000000000000000";

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
    }

    [Fact]
    public async Task ListPosts_ReturnsOnlyPublished_InSnakeCase()
    {
        var client = CreateClient(out var dbName, out _);
        await SeedPostsAsync(dbName);

        var response = await client.GetAsync("/cms/posts");

        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        using var body = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
        var post = Assert.Single(body.RootElement.EnumerateArray());
        Assert.Equal("hello", post.GetProperty("slug").GetString());
        Assert.Equal("a,b", post.GetProperty("tags").GetString());
        Assert.Equal("news", post.GetProperty("category").GetString());
        Assert.Equal("/uploads/x.png", post.GetProperty("cover_image").GetString());
        Assert.True(post.GetProperty("published").GetBoolean());
        Assert.Equal(JsonValueKind.String, post.GetProperty("published_at").ValueKind);
        Assert.Equal(JsonValueKind.String, post.GetProperty("created_at").ValueKind);
        Assert.Equal(JsonValueKind.String, post.GetProperty("updated_at").ValueKind);
    }

    [Fact]
    public async Task GetPostBySlug_PublishedOk_UnpublishedNotFound()
    {
        var client = CreateClient(out var dbName, out _);
        await SeedPostsAsync(dbName);

        Assert.Equal(HttpStatusCode.OK, (await client.GetAsync("/cms/posts/hello")).StatusCode);
        Assert.Equal(HttpStatusCode.NotFound, (await client.GetAsync("/cms/posts/draft")).StatusCode);
        Assert.Equal(HttpStatusCode.NotFound, (await client.GetAsync("/cms/posts/missing")).StatusCode);
    }

    [Fact]
    public async Task CreateOrder_MissingNameOrPhone_ReturnsBadRequest()
    {
        var client = CreateClient(out var dbName, out _);

        var noName = await client.PostAsJsonContent("/cms/orders", new { customer_name = " ", phone = "0900", quantity = 1 });
        var noPhone = await client.PostAsJsonContent("/cms/orders", new { customer_name = "An", quantity = 1 });

        Assert.Equal(HttpStatusCode.BadRequest, noName.StatusCode);
        Assert.Equal("customer_name and phone are required", await noName.Content.ReadAsStringAsync());
        Assert.Equal(HttpStatusCode.BadRequest, noPhone.StatusCode);
        using var db = OpenDb(dbName);
        Assert.Empty(db.CmsOrders);
    }

    [Fact]
    public async Task CreateOrder_Valid_StoresWithStatusNew()
    {
        var client = CreateClient(out var dbName, out _);

        var response = await client.PostAsJsonContent("/cms/orders", new
        {
            customer_name = "An", phone = "0900", email = (string?)null, address = "Hanoi", quantity = 2, note = "hi",
        });

        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        using var body = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
        Assert.Equal("new", body.RootElement.GetProperty("status").GetString());
        Assert.Equal("An", body.RootElement.GetProperty("customer_name").GetString());
        Assert.Equal(2, body.RootElement.GetProperty("quantity").GetInt64());
        using var db = OpenDb(dbName);
        Assert.Single(db.CmsOrders);
    }

    [Theory]
    [InlineData("GET", "/cms/admin/posts")]
    [InlineData("GET", "/cms/admin/orders")]
    [InlineData("GET", "/cms/auth/me")]
    public async Task AdminEndpoints_EnforceAdminRole(string method, string url)
    {
        var client = CreateClient(out _, out _);

        var anonymous = await client.SendAsync(new HttpRequestMessage(new HttpMethod(method), url));
        Assert.Equal(HttpStatusCode.Unauthorized, anonymous.StatusCode);

        var asUser = new HttpRequestMessage(new HttpMethod(method), url);
        asUser.Headers.Authorization = BearerFor(Guid.NewGuid(), UserRole.User);
        Assert.Equal(HttpStatusCode.Forbidden, (await client.SendAsync(asUser)).StatusCode);

        foreach (var role in new[] { UserRole.Admin, UserRole.SuperAdmin })
        {
            var asAdmin = new HttpRequestMessage(new HttpMethod(method), url);
            asAdmin.Headers.Authorization = BearerFor(Guid.NewGuid(), role);
            Assert.Equal(HttpStatusCode.OK, (await client.SendAsync(asAdmin)).StatusCode);
        }
    }

    [Fact]
    public async Task AdminPosts_CreateUpdateDelete()
    {
        var client = CreateClient(out var dbName, out _);
        client.DefaultRequestHeaders.Authorization = BearerFor(Guid.NewGuid(), UserRole.Admin);

        var created = await client.PostAsJsonContent("/cms/admin/posts", new
        {
            slug = "new-post", title = "New", excerpt = "e", content = "<p>c</p>", cover_image = (string?)null,
            category = "news", tags = "x,y", published = false,
        });
        Assert.Equal(HttpStatusCode.OK, created.StatusCode);
        using var createdBody = JsonDocument.Parse(await created.Content.ReadAsStringAsync());
        var id = createdBody.RootElement.GetProperty("id").GetGuid();
        Assert.Equal(JsonValueKind.Null, createdBody.RootElement.GetProperty("published_at").ValueKind);

        var duplicate = await client.PostAsJsonContent("/cms/admin/posts", new { slug = "new-post", title = "Dup", published = true });
        Assert.Equal(HttpStatusCode.BadRequest, duplicate.StatusCode);

        var updated = await client.PutAsync($"/cms/admin/posts/{id}", Json(new
        {
            id, slug = "renamed", title = "Renamed", excerpt = "e", content = "c", cover_image = "/uploads/a.png",
            category = (string?)null, tags = "", published = true, created_at = "ignored",
        }));
        Assert.Equal(HttpStatusCode.OK, updated.StatusCode);
        using (var updatedBody = JsonDocument.Parse(await updated.Content.ReadAsStringAsync()))
        {
            Assert.Equal("renamed", updatedBody.RootElement.GetProperty("slug").GetString());
            Assert.Equal(JsonValueKind.String, updatedBody.RootElement.GetProperty("published_at").ValueKind);
        }
        Assert.Equal(HttpStatusCode.OK, (await client.GetAsync($"/cms/admin/posts/{id}")).StatusCode);
        Assert.Equal(HttpStatusCode.OK, (await client.GetAsync("/cms/posts/renamed")).StatusCode);

        Assert.Equal(HttpStatusCode.NotFound,
            (await client.PutAsync($"/cms/admin/posts/{Guid.NewGuid()}", Json(new { slug = "z", title = "z", published = false }))).StatusCode);

        Assert.Equal(HttpStatusCode.NoContent, (await client.DeleteAsync($"/cms/admin/posts/{id}")).StatusCode);
        Assert.Equal(HttpStatusCode.NotFound, (await client.DeleteAsync($"/cms/admin/posts/{id}")).StatusCode);
        using var db = OpenDb(dbName);
        Assert.Empty(db.CmsPosts);
    }

    [Fact]
    public async Task AdminOrders_UpdateStatus()
    {
        var client = CreateClient(out var dbName, out _);
        var orderId = Guid.NewGuid();
        using (var db = OpenDb(dbName))
        {
            db.CmsOrders.Add(new CmsOrder { Id = orderId, CustomerName = "An", Phone = "0900", CreatedAt = DateTimeOffset.UtcNow });
            await db.SaveChangesAsync();
        }
        client.DefaultRequestHeaders.Authorization = BearerFor(Guid.NewGuid(), UserRole.Admin);

        var ok = await client.PostAsJsonContent($"/cms/admin/orders/{orderId}/status", new { status = "done" });
        var bad = await client.PostAsJsonContent($"/cms/admin/orders/{orderId}/status", new { status = "bogus" });
        var missing = await client.PostAsJsonContent($"/cms/admin/orders/{Guid.NewGuid()}/status", new { status = "done" });

        Assert.Equal(HttpStatusCode.OK, ok.StatusCode);
        using (var body = JsonDocument.Parse(await ok.Content.ReadAsStringAsync()))
        {
            Assert.Equal("done", body.RootElement.GetProperty("status").GetString());
        }
        Assert.Equal(HttpStatusCode.BadRequest, bad.StatusCode);
        Assert.Equal(HttpStatusCode.NotFound, missing.StatusCode);
        using var check = OpenDb(dbName);
        Assert.Equal("done", check.CmsOrders.Single().Status);
    }

    [Fact]
    public async Task Upload_RejectsNonImage_AcceptsImageAndServesIt()
    {
        var client = CreateClient(out _, out var uploadsDir);
        client.DefaultRequestHeaders.Authorization = BearerFor(Guid.NewGuid(), UserRole.Admin);

        var rejected = await client.PostAsync("/cms/admin/uploads", FileUpload([1, 2, 3], "text/plain", "a.txt"));
        Assert.Equal(HttpStatusCode.BadRequest, rejected.StatusCode);
        Assert.Empty(Directory.GetFiles(uploadsDir));

        byte[] png = [0x89, 0x50, 0x4E, 0x47, 1, 2, 3];
        var accepted = await client.PostAsync("/cms/admin/uploads", FileUpload(png, "image/png", "a.png"));
        Assert.Equal(HttpStatusCode.OK, accepted.StatusCode);
        using var body = JsonDocument.Parse(await accepted.Content.ReadAsStringAsync());
        var url = body.RootElement.GetProperty("url").GetString()!;
        Assert.Matches(@"^/uploads/[0-9a-f-]{36}\.png$", url);

        var served = await client.GetAsync("/cms" + url);
        Assert.Equal(HttpStatusCode.OK, served.StatusCode);
        Assert.Equal("image/png", served.Content.Headers.ContentType?.MediaType);
        Assert.Equal(png, await served.Content.ReadAsByteArrayAsync());
        Assert.Equal(HttpStatusCode.NotFound, (await client.GetAsync("/cms/uploads/..%2Fsecret.png")).StatusCode);
    }

    [Fact]
    public async Task Login_WithSynoraAdminAccount_SetsCookieSession()
    {
        var client = CreateClient(out var dbName, out _);
        await SeedUserAsync(dbName, "boss", "s3cret!", UserRole.Admin);
        await SeedUserAsync(dbName, "plain", "s3cret!", UserRole.User);

        Assert.Equal(HttpStatusCode.Unauthorized,
            (await client.PostAsJsonContent("/cms/auth/login", new { username = "boss", password = "wrong" })).StatusCode);
        Assert.Equal(HttpStatusCode.Unauthorized,
            (await client.PostAsJsonContent("/cms/auth/login", new { username = "nobody", password = "x" })).StatusCode);
        Assert.Equal(HttpStatusCode.Forbidden,
            (await client.PostAsJsonContent("/cms/auth/login", new { username = "plain", password = "s3cret!" })).StatusCode);
        Assert.Equal(HttpStatusCode.Unauthorized, (await client.GetAsync("/cms/auth/me")).StatusCode);

        var login = await client.PostAsJsonContent("/cms/auth/login", new { username = "boss", password = "s3cret!" });
        Assert.Equal(HttpStatusCode.OK, login.StatusCode);
        using (var body = JsonDocument.Parse(await login.Content.ReadAsStringAsync()))
        {
            Assert.Equal("boss", body.RootElement.GetProperty("username").GetString());
        }
        var setCookie = Assert.Single(login.Headers.GetValues("Set-Cookie"));
        Assert.StartsWith("synora_auth=", setCookie);
        Assert.Contains("path=/", setCookie, StringComparison.OrdinalIgnoreCase);

        // The test client keeps cookies, so the session carries over.
        Assert.Equal(HttpStatusCode.OK, (await client.GetAsync("/cms/auth/me")).StatusCode);
        Assert.Equal(HttpStatusCode.OK, (await client.GetAsync("/cms/admin/orders")).StatusCode);

        Assert.Equal(HttpStatusCode.NoContent, (await client.PostAsync("/cms/auth/logout", null)).StatusCode);
        Assert.Equal(HttpStatusCode.Unauthorized, (await client.GetAsync("/cms/auth/me")).StatusCode);
    }

    [Fact]
    public async Task LegacyImport_CopiesSqliteDataAndUploads_ThenNoOps()
    {
        var legacyDir = CreateTempDir();
        var uploadsDir = CreateTempDir();
        var sqlitePath = Path.Combine(legacyDir, "synora_cms.sqlite3");
        Directory.CreateDirectory(Path.Combine(legacyDir, "uploads"));
        File.WriteAllBytes(Path.Combine(legacyDir, "uploads", "d29a8a68-832a-4179-9881-161d0503b5f9.png"), [1, 2, 3]);
        var postId = Guid.NewGuid();
        var orderId = Guid.NewGuid();
        CreateLegacySqlite(sqlitePath, postId, orderId);

        var options = new CmsOptions(uploadsDir, sqlitePath, Path.Combine(legacyDir, "uploads"));
        var dbName = Guid.NewGuid().ToString();

        using (var db = OpenDb(dbName))
        {
            var result = await CmsLegacyImporter.ImportAsync(db, options, NullLogger.Instance);
            Assert.Equal(new CmsImportResult(true, 2, 1, 1), result);
        }
        using (var db = OpenDb(dbName))
        {
            var post = db.CmsPosts.Single(p => p.Id == postId);
            Assert.Equal("hello", post.Slug);
            Assert.True(post.Published);
            Assert.Equal("news", post.Category);
            Assert.Equal("a,b", post.Tags);
            Assert.Equal("/uploads/d29a8a68-832a-4179-9881-161d0503b5f9.png", post.CoverImage);
            Assert.Equal(new DateTimeOffset(2026, 10, 13, 16, 29, 33, TimeSpan.Zero).AddTicks(7025843), post.PublishedAt);
            Assert.Null(db.CmsPosts.Single(p => p.Slug == "draft").PublishedAt);
            var order = db.CmsOrders.Single();
            Assert.Equal(orderId, order.Id);
            Assert.Equal("cancelled", order.Status);
            Assert.Equal(3, order.Quantity);
        }
        Assert.True(File.Exists(Path.Combine(uploadsDir, "d29a8a68-832a-4179-9881-161d0503b5f9.png")));

        using (var db = OpenDb(dbName))
        {
            var second = await CmsLegacyImporter.ImportAsync(db, options, NullLogger.Instance);
            Assert.False(second.Imported);
            Assert.Equal(2, db.CmsPosts.Count());
            Assert.Equal(1, db.CmsOrders.Count());
        }

        var missing = options with { LegacySqlitePath = Path.Combine(legacyDir, "nope.sqlite3") };
        using (var db = OpenDb(Guid.NewGuid().ToString()))
        {
            Assert.False((await CmsLegacyImporter.ImportAsync(db, missing, NullLogger.Instance)).Imported);
        }
    }

    // Schema = the Rust service's migrations 0001 + 0002.
    private static void CreateLegacySqlite(string path, Guid postId, Guid orderId)
    {
        using var conn = new SqliteConnection(new SqliteConnectionStringBuilder { DataSource = path, Pooling = false }.ToString());
        conn.Open();
        using var cmd = conn.CreateCommand();
        cmd.CommandText = $"""
            CREATE TABLE posts (id TEXT PRIMARY KEY, slug TEXT NOT NULL UNIQUE, title TEXT NOT NULL,
                excerpt TEXT NOT NULL DEFAULT '', content TEXT NOT NULL DEFAULT '', cover_image TEXT,
                published INTEGER NOT NULL DEFAULT 0, published_at TEXT, created_at TEXT NOT NULL, updated_at TEXT NOT NULL);
            CREATE TABLE orders (id TEXT PRIMARY KEY, customer_name TEXT NOT NULL, phone TEXT NOT NULL, email TEXT,
                address TEXT, quantity INTEGER NOT NULL DEFAULT 1, note TEXT, status TEXT NOT NULL DEFAULT 'new',
                created_at TEXT NOT NULL);
            ALTER TABLE posts ADD COLUMN category TEXT;
            ALTER TABLE posts ADD COLUMN tags TEXT NOT NULL DEFAULT '';
            INSERT INTO posts VALUES ('{postId}', 'hello', 'Hello', 'ex', '<img src="/uploads/d29a8a68-832a-4179-9881-161d0503b5f9.png">',
                '/uploads/d29a8a68-832a-4179-9881-161d0503b5f9.png', 1, '2026-10-13T16:29:33.702584369+00:00',
                '2026-10-13T16:29:33.702584369+00:00', '2026-10-13T16:29:33.702584369+00:00', 'news', 'a,b');
            INSERT INTO posts VALUES ('{Guid.NewGuid()}', 'draft', 'Draft', '', '', NULL, 0, NULL,
                '2026-10-14T08:00:00+00:00', '2026-10-14T08:00:00+00:00', NULL, '');
            INSERT INTO orders VALUES ('{orderId}', 'An', '0900', NULL, 'Hanoi', 3, NULL, 'cancelled',
                '2026-10-13T16:40:26.692633065+00:00');
            """;
        cmd.ExecuteNonQuery();
    }

    private static async Task SeedPostsAsync(string dbName)
    {
        using var db = OpenDb(dbName);
        var now = DateTimeOffset.UtcNow;
        db.CmsPosts.AddRange(
            new CmsPost
            {
                Id = Guid.NewGuid(), Slug = "hello", Title = "Hello", Category = "news", Tags = "a,b",
                CoverImage = "/uploads/x.png", Published = true, PublishedAt = now, CreatedAt = now, UpdatedAt = now,
            },
            new CmsPost
            {
                Id = Guid.NewGuid(), Slug = "draft", Title = "Draft", Published = false, CreatedAt = now, UpdatedAt = now,
            });
        await db.SaveChangesAsync();
    }

    private static async Task SeedUserAsync(string dbName, string username, string password, UserRole role)
    {
        using var db = OpenDb(dbName);
        var (hash, salt) = PasswordHasher.Hash(password);
        db.Users.Add(new User
        {
            Id = Guid.NewGuid(), Username = username, PasswordHash = hash, PasswordSalt = salt, Role = role,
            CreatedAt = DateTimeOffset.UtcNow,
        });
        await db.SaveChangesAsync();
    }

    private static MultipartFormDataContent FileUpload(byte[] bytes, string contentType, string fileName)
    {
        var content = new MultipartFormDataContent();
        var file = new ByteArrayContent(bytes);
        file.Headers.ContentType = new MediaTypeHeaderValue(contentType);
        content.Add(file, "file", fileName);
        return content;
    }

    private static StringContent Json<T>(T value) =>
        new(JsonSerializer.Serialize(value), Encoding.UTF8, "application/json");

    private HttpClient CreateClient(out string dbName, out string uploadsDir)
    {
        dbName = Guid.NewGuid().ToString();
        var capturedDbName = dbName;
        var capturedUploadsDir = uploadsDir = CreateTempDir();
        var app = factory.WithWebHostBuilder(builder =>
        {
            builder.UseEnvironment("Testing");
            builder.ConfigureAppConfiguration(config => config.AddInMemoryCollection(
                new Dictionary<string, string?> { ["Cms:UploadsPath"] = capturedUploadsDir }));
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
        return app.CreateClient();
    }

    private static readonly InMemoryDatabaseRoot SharedDatabaseRoot = new();

    private static SynoraDbContext OpenDb(string dbName) =>
        new(new DbContextOptionsBuilder<SynoraDbContext>().UseInMemoryDatabase(dbName, SharedDatabaseRoot).Options);

    private string CreateTempDir()
    {
        var dir = Path.Combine(Path.GetTempPath(), "synora-cms-tests-" + Guid.NewGuid());
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
            new SymmetricSecurityKey(Encoding.UTF8.GetBytes(JwtFallbackSigningKey)), SecurityAlgorithms.HmacSha256);
        var token = new JwtSecurityToken(
            issuer: "synora", audience: "synora-mobile", claims: claims,
            expires: DateTime.UtcNow.AddHours(1), signingCredentials: credentials);
        return new AuthenticationHeaderValue("Bearer", new JwtSecurityTokenHandler().WriteToken(token));
    }
}
