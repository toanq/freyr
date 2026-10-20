using System.Security.Claims;
using System.Text.RegularExpressions;
using Microsoft.AspNetCore.Authentication;
using Microsoft.AspNetCore.Authentication.Cookies;
using Microsoft.AspNetCore.Mvc;
using Microsoft.AspNetCore.StaticFiles;
using Microsoft.EntityFrameworkCore;
using Synora.Api.Auth;
using Synora.Core;
using Synora.Infrastructure;

namespace Synora.Api.Cms;

// Landing-site CMS (served at / on synora.id.vn), ported from the former
// standalone Rust service (landing/server). Every route sits under /cms; the
// landing nginx maps its public /api/* -> /cms/* and /uploads/* ->
// /cms/uploads/*, so the landing frontend's paths and JSON are unchanged.
//
// Differences from the Rust service: admin login uses Synora accounts
// (Admin/SuperAdmin only) and Synora's own auth cookie instead of a separate
// admin_users/sessions store; error bodies stay plain text (the frontend
// shows them verbatim).
public static partial class CmsEndpoints
{
    private const long MaxUploadBytes = 8 * 1024 * 1024;
    private const long MaxOrderRequestBytes = 16 * 1024;

    public static WebApplication MapCms(this WebApplication app)
    {
        var options = CmsOptions.From(app.Configuration);
        Directory.CreateDirectory(options.UploadsPath);

        if (!app.Environment.IsEnvironment("Testing"))
        {
            // Runs after Program.cs's db.Database.Migrate(), so the CMS tables exist.
            using var scope = app.Services.CreateScope();
            var db = scope.ServiceProvider.GetRequiredService<SynoraDbContext>();
            var logger = scope.ServiceProvider.GetRequiredService<ILoggerFactory>().CreateLogger("Synora.Api.Cms.LegacyImport");
            try
            {
                CmsLegacyImporter.ImportAsync(db, options, logger).GetAwaiter().GetResult();
            }
            catch (Exception ex)
            {
                // Tables are still empty (single SaveChanges), so the next startup retries.
                logger.LogError(ex, "Legacy CMS import from {Path} failed; continuing without it.", options.LegacySqlitePath);
            }
        }

        var cms = app.MapGroup("/cms");
        MapPublic(cms, options);
        MapAuth(cms);
        MapAdmin(cms, options);
        return app;
    }

    private static void MapPublic(RouteGroupBuilder cms, CmsOptions options)
    {
        cms.MapGet("/posts", async (SynoraDbContext db) =>
        {
            var posts = await db.CmsPosts.AsNoTracking()
                .Where(p => p.Published)
                .OrderByDescending(p => p.PublishedAt).ThenByDescending(p => p.CreatedAt)
                .ToListAsync();
            return Results.Ok(posts.Select(CmsPostDto.From));
        });

        cms.MapGet("/posts/{slug}", async (SynoraDbContext db, string slug) =>
        {
            var post = await db.CmsPosts.AsNoTracking().FirstOrDefaultAsync(p => p.Published && p.Slug == slug);
            return post is null ? Results.NotFound() : Results.Ok(CmsPostDto.From(post));
        });

        cms.MapPost("/orders", async (SynoraDbContext db, CmsOrderRequest body) =>
        {
            if (ValidateOrder(body) is { } error)
            {
                return Text(StatusCodes.Status400BadRequest, error);
            }
            var order = new CmsOrder
            {
                Id = Guid.NewGuid(),
                CustomerName = body.CustomerName!,
                Phone = body.Phone!,
                Email = body.Email,
                Address = body.Address,
                Quantity = body.Quantity,
                Note = body.Note,
                Status = CmsOrderStatuses.New,
                CreatedAt = DateTimeOffset.UtcNow,
            };
            db.CmsOrders.Add(order);
            await db.SaveChangesAsync();
            return Results.Ok(CmsOrderDto.From(order));
        }).WithMetadata(new RequestSizeLimitAttribute(MaxOrderRequestBytes));

        // Uploaded images, served by name. Only server-generated-looking
        // names are accepted, so nothing outside UploadsPath is reachable.
        var contentTypes = new FileExtensionContentTypeProvider();
        cms.MapGet("/uploads/{fileName}", (HttpContext http, string fileName) =>
        {
            if (!SafeUploadName().IsMatch(fileName))
            {
                return Results.NotFound();
            }
            var fullPath = Path.GetFullPath(Path.Combine(options.UploadsPath, fileName));
            if (!File.Exists(fullPath))
            {
                return Results.NotFound();
            }
            if (!contentTypes.TryGetContentType(fileName, out var contentType))
            {
                contentType = "application/octet-stream";
            }
            http.Response.Headers.XContentTypeOptions = "nosniff";
            // Names are random UUIDs that never get rewritten -- safe to cache long.
            http.Response.Headers.CacheControl = "public, max-age=31536000, immutable";
            return Results.File(fullPath, contentType, lastModified: File.GetLastWriteTimeUtc(fullPath), enableRangeProcessing: true);
        });
    }

    private static void MapAuth(RouteGroupBuilder cms)
    {
        // Same credential check and cookie as Synora's POST /auth/login, but
        // only Admin/SuperAdmin may sign in here.
        cms.MapPost("/auth/login", async (HttpContext http, SynoraDbContext db, CmsLoginRequest body) =>
        {
            if (string.IsNullOrEmpty(body.Username) || body.Password is null)
            {
                return Text(StatusCodes.Status401Unauthorized, "invalid credentials");
            }
            var user = await db.Users.FirstOrDefaultAsync(u => u.Username == body.Username);
            if (!SessionAuth.VerifyPassword(user, body.Password))
            {
                return Text(StatusCodes.Status401Unauthorized, "invalid credentials");
            }
            if (!SessionAuth.IsAdmin(user!.Role))
            {
                return Text(StatusCodes.Status403Forbidden, "admin access required");
            }

            await http.SignInAsync(
                CookieAuthenticationDefaults.AuthenticationScheme,
                SessionAuth.BuildPrincipal(user, CookieAuthenticationDefaults.AuthenticationScheme));
            return Results.Ok(new CmsLoginResponse(user.Username));
        });

        cms.MapPost("/auth/logout", async (HttpContext http) =>
        {
            await http.SignOutAsync(CookieAuthenticationDefaults.AuthenticationScheme);
            return Results.NoContent();
        });

        cms.MapGet("/auth/me", (HttpContext http) => Results.Ok(new CmsMeResponse(
                http.User.FindFirstValue(ClaimTypes.NameIdentifier) ?? "",
                http.User.Identity?.Name)))
            .RequireAuthorization()
            .AddEndpointFilter(RequireAdmin);
    }

    private static void MapAdmin(RouteGroupBuilder cms, CmsOptions options)
    {
        // Default policy = authenticated via Synora cookie or JWT (401
        // otherwise); the filter then requires Admin/SuperAdmin (403).
        var admin = cms.MapGroup("/admin").RequireAuthorization().AddEndpointFilter(RequireAdmin);

        admin.MapGet("/posts", async (SynoraDbContext db) =>
        {
            var posts = await db.CmsPosts.AsNoTracking().OrderByDescending(p => p.CreatedAt).ToListAsync();
            return Results.Ok(posts.Select(CmsPostDto.From));
        });

        admin.MapGet("/posts/{id}", async (SynoraDbContext db, string id) =>
        {
            var post = Guid.TryParse(id, out var guid) ? await db.CmsPosts.FindAsync(guid) : null;
            return post is null ? Results.NotFound() : Results.Ok(CmsPostDto.From(post));
        });

        admin.MapPost("/posts", async (SynoraDbContext db, CmsPostRequest body) =>
        {
            if (await ValidatePostAsync(db, body, existingId: null) is { } error)
            {
                return Text(StatusCodes.Status400BadRequest, $"could not create post: {error}");
            }
            var now = DateTimeOffset.UtcNow;
            var post = new CmsPost { Slug = "", Title = "", Id = Guid.NewGuid(), CreatedAt = now };
            Apply(post, body, now);
            db.CmsPosts.Add(post);
            await db.SaveChangesAsync();
            return Results.Ok(CmsPostDto.From(post));
        });

        admin.MapPut("/posts/{id}", async (SynoraDbContext db, string id, CmsPostRequest body) =>
        {
            var post = Guid.TryParse(id, out var guid) ? await db.CmsPosts.FindAsync(guid) : null;
            if (post is null)
            {
                return Text(StatusCodes.Status404NotFound, "post not found");
            }
            if (await ValidatePostAsync(db, body, existingId: post.Id) is { } error)
            {
                return Text(StatusCodes.Status400BadRequest, $"could not update post: {error}");
            }
            Apply(post, body, DateTimeOffset.UtcNow);
            await db.SaveChangesAsync();
            return Results.Ok(CmsPostDto.From(post));
        });

        admin.MapDelete("/posts/{id}", async (SynoraDbContext db, string id) =>
        {
            var post = Guid.TryParse(id, out var guid) ? await db.CmsPosts.FindAsync(guid) : null;
            if (post is null)
            {
                return Results.NotFound();
            }
            db.CmsPosts.Remove(post);
            await db.SaveChangesAsync();
            return Results.NoContent();
        });

        admin.MapGet("/orders", async (SynoraDbContext db) =>
        {
            var orders = await db.CmsOrders.AsNoTracking().OrderByDescending(o => o.CreatedAt).ToListAsync();
            return Results.Ok(orders.Select(CmsOrderDto.From));
        });

        admin.MapPost("/orders/{id}/status", async (SynoraDbContext db, string id, CmsOrderStatusRequest body) =>
        {
            var order = Guid.TryParse(id, out var guid) ? await db.CmsOrders.FindAsync(guid) : null;
            if (order is null)
            {
                return Results.NotFound();
            }
            if (body.Status is null || !CmsOrderStatuses.All.Contains(body.Status))
            {
                return Text(StatusCodes.Status400BadRequest, $"status must be one of: {string.Join(", ", CmsOrderStatuses.All)}");
            }
            order.Status = body.Status;
            await db.SaveChangesAsync();
            return Results.Ok(CmsOrderDto.From(order));
        });

        // Form read by hand (not an IFormFile parameter): IFormFile binding
        // attaches antiforgery metadata, and no antiforgery middleware is
        // registered in this app. First file part wins, whatever its field
        // name, as in the Rust service.
        admin.MapPost("/uploads", async (HttpContext http) =>
        {
            if (!http.Request.HasFormContentType)
            {
                return Text(StatusCodes.Status400BadRequest, "no file provided");
            }
            var form = await http.Request.ReadFormAsync();
            var file = form.Files.FirstOrDefault();
            if (file is null)
            {
                return Text(StatusCodes.Status400BadRequest, "no file provided");
            }
            if (ExtensionFor(file.ContentType) is not { } ext)
            {
                return Text(StatusCodes.Status400BadRequest, "unsupported image type");
            }
            if (file.Length > MaxUploadBytes)
            {
                return Text(StatusCodes.Status413PayloadTooLarge, "image too large");
            }

            Directory.CreateDirectory(options.UploadsPath);
            var fileName = $"{Guid.NewGuid()}.{ext}";
            await using (var stream = File.Create(Path.Combine(options.UploadsPath, fileName)))
            {
                await file.CopyToAsync(stream);
            }
            return Results.Ok(new CmsUploadResponse($"/uploads/{fileName}"));
        }).WithMetadata(new RequestSizeLimitAttribute(MaxUploadBytes + 64 * 1024));
    }

    private static ValueTask<object?> RequireAdmin(EndpointFilterInvocationContext context, EndpointFilterDelegate next) =>
        SessionAuth.IsAdmin(context.HttpContext.User) ? next(context) : ValueTask.FromResult<object?>(Results.Forbid());

    private static void Apply(CmsPost post, CmsPostRequest body, DateTimeOffset now)
    {
        post.Slug = body.Slug!.Trim();
        post.Title = body.Title!;
        post.Excerpt = body.Excerpt ?? "";
        post.Content = body.Content ?? "";
        post.CoverImage = body.CoverImage;
        post.Category = body.Category;
        post.Tags = body.Tags ?? "";
        // Keep the original publish date across edits; reset on unpublish.
        post.PublishedAt = body.Published ? post.PublishedAt ?? now : null;
        post.Published = body.Published;
        post.UpdatedAt = now;
    }

    private static async Task<string?> ValidatePostAsync(SynoraDbContext db, CmsPostRequest body, Guid? existingId)
    {
        if (string.IsNullOrWhiteSpace(body.Slug))
        {
            return "slug is required";
        }
        if (body.Slug.Trim().Contains('/'))
        {
            return "slug must not contain '/'";
        }
        if (body.Title is null)
        {
            return "title is required";
        }
        var slug = body.Slug.Trim();
        if (await db.CmsPosts.AnyAsync(p => p.Slug == slug && p.Id != existingId))
        {
            return "slug already exists";
        }
        return null;
    }

    private static string? ValidateOrder(CmsOrderRequest body)
    {
        // Same required fields as the Rust service, plus length caps.
        if (string.IsNullOrWhiteSpace(body.CustomerName) || string.IsNullOrWhiteSpace(body.Phone))
        {
            return "customer_name and phone are required";
        }
        if (body.CustomerName.Length > 200 || body.Phone.Length > 50 || body.Email?.Length > 254 ||
            body.Address?.Length > 1000 || body.Note?.Length > 4000)
        {
            return "a field is too long";
        }
        if (body.Quantity is < 1 or > 10_000)
        {
            return "quantity must be between 1 and 10000";
        }
        return null;
    }

    private static string? ExtensionFor(string? contentType) =>
        contentType?.Split(';')[0].Trim().ToLowerInvariant() switch
        {
            "image/png" => "png",
            "image/jpeg" => "jpg",
            "image/webp" => "webp",
            "image/gif" => "gif",
            _ => null,
        };

    private static IResult Text(int statusCode, string message) =>
        Results.Text(message, "text/plain; charset=utf-8", statusCode: statusCode);

    [GeneratedRegex(@"^[A-Za-z0-9][A-Za-z0-9_-]*\.[A-Za-z0-9]+$")]
    internal static partial Regex SafeUploadName();
}

public sealed record CmsOptions(string UploadsPath, string LegacySqlitePath, string LegacyUploadsPath)
{
    public static CmsOptions From(IConfiguration configuration)
    {
        var legacySqlite = configuration["Cms:LegacySqlitePath"] ?? "/data/landing-legacy/synora_cms.sqlite3";
        // The Rust service kept uploads in data/uploads next to its SQLite file.
        var legacyUploads = configuration["Cms:LegacyUploadsPath"]
            ?? Path.Combine(Path.GetDirectoryName(legacySqlite) ?? ".", "uploads");
        return new CmsOptions(configuration["Cms:UploadsPath"] ?? "/data/cms-uploads", legacySqlite, legacyUploads);
    }
}
