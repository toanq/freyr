using Microsoft.EntityFrameworkCore;
using Synora.Api.Mqtt;
using Synora.Core;
using Synora.Infrastructure;

namespace Synora.Api.Devices;

internal record AdminUserLookupDto(Guid Id, string Username, string? Email);
internal record SetDeviceOwnerRequest(Guid? UserId);

// Admin-only: link a device to a user account (assign / change / clear).
// /users is SuperAdminOnly, so plain Admins get a minimal user lookup here
// to pick the account from.
internal static class AdminDeviceOwnerEndpoints
{
    private const int LookupLimit = 20;

    public static WebApplication MapAdminDeviceOwner(this WebApplication app, string avatarStoragePath)
    {
        var admin = app.MapGroup("/admin").RequireAuthorization();

        admin.MapGet("/users/lookup", async (string? q, SynoraDbContext db, HttpContext http) =>
        {
            if (!IsAdmin(http))
            {
                return Results.Forbid();
            }
            var term = q?.Trim().ToLowerInvariant() ?? "";
            if (term.Length < 2)
            {
                return Results.BadRequest("Query must be at least 2 characters.");
            }

            var users = await db.Users
                .Where(u => u.Username.ToLower().Contains(term) || (u.Email != null && u.Email.ToLower().Contains(term)))
                .OrderBy(u => u.Username)
                .Take(LookupLimit)
                .Select(u => new AdminUserLookupDto(u.Id, u.Username, u.Email))
                .ToListAsync();
            return Results.Ok(users);
        });

        admin.MapPut("/devices/{id:guid}/owner", async (
            Guid id, SetDeviceOwnerRequest request, SynoraDbContext db, HttpContext http, IDeviceMqttPublisher mqtt) =>
        {
            if (!IsAdmin(http))
            {
                return Results.Forbid();
            }
            var device = await db.Devices.FirstOrDefaultAsync(d => d.Id == id);
            if (device is null)
            {
                return Results.NotFound("Device not found.");
            }
            if (request.UserId is { } userId && !await db.Users.AnyAsync(u => u.Id == userId))
            {
                return Results.NotFound("User not found.");
            }

            await DeviceOwnership.SetOwnerAsync(db, mqtt, device, request.UserId, avatarStoragePath);

            var summary = (await DeviceSummaries.ListAsync(db, db.Devices.Where(d => d.Id == id))).Single();
            return Results.Ok(summary);
        });

        return app;
    }

    private static bool IsAdmin(HttpContext http) =>
        http.User.IsInRole(nameof(UserRole.Admin)) || http.User.IsInRole(nameof(UserRole.SuperAdmin));
}
