using Microsoft.EntityFrameworkCore;
using Synora.Api.Auth;
using Synora.Infrastructure;

namespace Synora.Api.Firmware;

// Admin-only reads of a build's flash kit, used by the web console's USB
// flasher. Devices never call these (OTA uses the unauthenticated
// /firmware/{id}/binary app image instead).
public static class FlashKitEndpoints
{
    public static string KitDir(string storagePath, Guid buildId) => Path.Combine(storagePath, buildId.ToString());

    public static void MapFlashKit(this RouteGroupBuilder adminGroup, string storagePath)
    {
        adminGroup.MapGet("/firmware/{id:guid}/flash-manifest", async (Guid id, HttpContext http, SynoraDbContext db) =>
        {
            if (!SessionAuth.IsAdmin(http.User)) return Results.Forbid();
            var build = await db.FirmwareBuilds.AsNoTracking().FirstOrDefaultAsync(f => f.Id == id);
            if (build is null || !build.HasFlashKit) return Results.NotFound();

            var manifest = await FlashKit.ReadManifestAsync(KitDir(storagePath, id));
            return manifest is null ? Results.NotFound() : Results.Ok(manifest);
        });

        // {*name} because kit paths contain '/' (e.g. bootloader/bootloader.bin).
        // Only names listed in the server-written manifest resolve, and they map
        // to server-chosen file names -- the request path never touches the disk.
        adminGroup.MapGet("/firmware/{id:guid}/flash-files/{*name}", async (Guid id, string name, HttpContext http, SynoraDbContext db) =>
        {
            if (!SessionAuth.IsAdmin(http.User)) return Results.Forbid();
            var build = await db.FirmwareBuilds.AsNoTracking().FirstOrDefaultAsync(f => f.Id == id);
            if (build is null || !build.HasFlashKit) return Results.NotFound();

            var kitDir = KitDir(storagePath, id);
            var manifest = await FlashKit.ReadManifestAsync(kitDir);
            var file = manifest?.Files.FirstOrDefault(f => f.Name == name);
            if (file is null) return Results.NotFound();

            var fullPath = Path.Combine(kitDir, file.StoredAs);
            return File.Exists(fullPath)
                ? Results.File(fullPath, "application/octet-stream")
                : Results.NotFound();
        });
    }
}
