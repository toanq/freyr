using System.Globalization;
using System.Text.RegularExpressions;
using Microsoft.Data.Sqlite;
using Microsoft.EntityFrameworkCore;
using Synora.Core;
using Synora.Infrastructure;

namespace Synora.Api.Cms;

public sealed record CmsImportResult(bool Imported, int Posts, int Orders, int Uploads);

// One-time import of the former Rust CMS's SQLite database (posts + orders)
// and its uploads folder into Postgres / Cms:UploadsPath. Runs at startup
// (CmsEndpoints.MapCms) and is a no-op when the legacy file is missing or
// either CMS table already has rows -- so it effectively runs once.
// Ids, slugs, timestamps, published flags, category, tags and status are
// preserved; image URLs (/uploads/<name>) are left untouched since uploads
// keep their file names and the landing nginx still serves /uploads/*.
public static partial class CmsLegacyImporter
{
    private static readonly CmsImportResult Skipped = new(false, 0, 0, 0);

    public static async Task<CmsImportResult> ImportAsync(
        SynoraDbContext db, CmsOptions options, ILogger logger, CancellationToken ct = default)
    {
        if (!File.Exists(options.LegacySqlitePath))
        {
            logger.LogDebug("No legacy CMS database at {Path}; skipping import.", options.LegacySqlitePath);
            return Skipped;
        }
        if (await db.CmsPosts.AnyAsync(ct) || await db.CmsOrders.AnyAsync(ct))
        {
            logger.LogDebug("CMS tables already populated; skipping legacy import.");
            return Skipped;
        }

        // Work on a copy: the legacy volume is mounted read-only, and a
        // WAL-mode SQLite file can't be opened from a read-only directory.
        // Copying the -wal alongside lets SQLite replay any unflushed pages.
        var workDir = Path.Combine(Path.GetTempPath(), "synora-cms-import-" + Guid.NewGuid());
        Directory.CreateDirectory(workDir);
        try
        {
            var dbCopy = Path.Combine(workDir, "legacy.sqlite3");
            File.Copy(options.LegacySqlitePath, dbCopy);
            if (File.Exists(options.LegacySqlitePath + "-wal"))
            {
                File.Copy(options.LegacySqlitePath + "-wal", dbCopy + "-wal");
            }

            List<CmsPost> posts;
            List<CmsOrder> orders;
            await using (var conn = new SqliteConnection(new SqliteConnectionStringBuilder
            {
                DataSource = dbCopy,
                Pooling = false,
            }.ToString()))
            {
                await conn.OpenAsync(ct);
                posts = await ReadPostsAsync(conn, logger, ct);
                orders = await ReadOrdersAsync(conn, logger, ct);
            }

            var uploads = CopyUploads(options, logger);

            db.CmsPosts.AddRange(posts);
            db.CmsOrders.AddRange(orders);
            await db.SaveChangesAsync(ct);

            logger.LogInformation(
                "Imported legacy CMS data from {Path}: {Posts} posts, {Orders} orders, {Uploads} upload files.",
                options.LegacySqlitePath, posts.Count, orders.Count, uploads);
            return new CmsImportResult(true, posts.Count, orders.Count, uploads);
        }
        finally
        {
            try
            {
                Directory.Delete(workDir, recursive: true);
            }
            catch (IOException)
            {
            }
        }
    }

    private static async Task<List<CmsPost>> ReadPostsAsync(SqliteConnection conn, ILogger logger, CancellationToken ct)
    {
        // category/tags arrived in the Rust service's second migration.
        var columns = await ColumnsAsync(conn, "posts", ct);
        var category = columns.Contains("category") ? "category" : "NULL";
        var tags = columns.Contains("tags") ? "tags" : "''";

        await using var cmd = conn.CreateCommand();
        cmd.CommandText =
            $"SELECT id, slug, title, excerpt, content, cover_image, {category}, {tags}, published, published_at, created_at, updated_at FROM posts";
        await using var reader = await cmd.ExecuteReaderAsync(ct);
        var posts = new List<CmsPost>();
        while (await reader.ReadAsync(ct))
        {
            var now = DateTimeOffset.UtcNow;
            posts.Add(new CmsPost
            {
                Id = ParseId(reader.GetString(0), logger),
                Slug = reader.GetString(1),
                Title = reader.GetString(2),
                Excerpt = StringOrNull(reader, 3) ?? "",
                Content = StringOrNull(reader, 4) ?? "",
                CoverImage = StringOrNull(reader, 5),
                Category = StringOrNull(reader, 6),
                Tags = StringOrNull(reader, 7) ?? "",
                Published = reader.GetInt64(8) != 0,
                PublishedAt = ParseTimestamp(StringOrNull(reader, 9)),
                CreatedAt = ParseTimestamp(StringOrNull(reader, 10)) ?? now,
                UpdatedAt = ParseTimestamp(StringOrNull(reader, 11)) ?? now,
            });
        }
        return posts;
    }

    private static async Task<List<CmsOrder>> ReadOrdersAsync(SqliteConnection conn, ILogger logger, CancellationToken ct)
    {
        await using var cmd = conn.CreateCommand();
        cmd.CommandText = "SELECT id, customer_name, phone, email, address, quantity, note, status, created_at FROM orders";
        await using var reader = await cmd.ExecuteReaderAsync(ct);
        var orders = new List<CmsOrder>();
        while (await reader.ReadAsync(ct))
        {
            orders.Add(new CmsOrder
            {
                Id = ParseId(reader.GetString(0), logger),
                CustomerName = reader.GetString(1),
                Phone = reader.GetString(2),
                Email = StringOrNull(reader, 3),
                Address = StringOrNull(reader, 4),
                Quantity = reader.GetInt64(5),
                Note = StringOrNull(reader, 6),
                Status = reader.GetString(7),
                CreatedAt = ParseTimestamp(StringOrNull(reader, 8)) ?? DateTimeOffset.UtcNow,
            });
        }
        return orders;
    }

    // Copies top-level files only, keeping their names (post content links
    // to /uploads/<name>); never overwrites, so a retried import is harmless.
    private static int CopyUploads(CmsOptions options, ILogger logger)
    {
        if (!Directory.Exists(options.LegacyUploadsPath))
        {
            logger.LogInformation("No legacy CMS uploads folder at {Path}.", options.LegacyUploadsPath);
            return 0;
        }
        Directory.CreateDirectory(options.UploadsPath);
        var copied = 0;
        foreach (var source in Directory.EnumerateFiles(options.LegacyUploadsPath))
        {
            var name = Path.GetFileName(source);
            if (!CmsEndpoints.SafeUploadName().IsMatch(name))
            {
                logger.LogWarning("Skipping legacy upload with unexpected name {Name}.", name);
                continue;
            }
            var destination = Path.Combine(options.UploadsPath, name);
            if (!File.Exists(destination))
            {
                File.Copy(source, destination);
                copied++;
            }
        }
        return copied;
    }

    private static async Task<HashSet<string>> ColumnsAsync(SqliteConnection conn, string table, CancellationToken ct)
    {
        await using var cmd = conn.CreateCommand();
        cmd.CommandText = $"PRAGMA table_info({table})";
        await using var reader = await cmd.ExecuteReaderAsync(ct);
        var columns = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        while (await reader.ReadAsync(ct))
        {
            columns.Add(reader.GetString(1));
        }
        return columns;
    }

    private static string? StringOrNull(SqliteDataReader reader, int ordinal) =>
        reader.IsDBNull(ordinal) ? null : reader.GetString(ordinal);

    private static Guid ParseId(string raw, ILogger logger)
    {
        if (Guid.TryParse(raw, out var id))
        {
            return id;
        }
        id = Guid.NewGuid();
        logger.LogWarning("Legacy CMS id {Raw} is not a UUID; imported as {Id}.", raw, id);
        return id;
    }

    // Rust's chrono writes RFC 3339 with nanoseconds (9 fractional digits);
    // .NET parses at most 7, so the extra digits are dropped first.
    internal static DateTimeOffset? ParseTimestamp(string? raw)
    {
        if (string.IsNullOrWhiteSpace(raw))
        {
            return null;
        }
        var trimmed = ExtraFractionDigits().Replace(raw.Trim(), "$1");
        return DateTimeOffset.TryParse(trimmed, CultureInfo.InvariantCulture, DateTimeStyles.AssumeUniversal, out var value)
            ? value.ToUniversalTime()
            : null;
    }

    [GeneratedRegex(@"(\.\d{7})\d+")]
    private static partial Regex ExtraFractionDigits();
}
