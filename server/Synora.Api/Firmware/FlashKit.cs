using System.Globalization;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Synora.Api.Firmware;

// A "flash kit" is the zip scripts/package-firmware.sh builds from an
// `idf.py build` output: flasher_args.json plus every image it references
// (bootloader, partition table, otadata, app, srmodels). The server keeps the
// referenced images under Firmware:StoragePath/<buildId>/ and a manifest the
// web console's USB flasher (esptool-js) reads to know what goes where.
public sealed record FlashKitFile(
    [property: JsonPropertyName("name")] string Name,
    [property: JsonPropertyName("offset")] long Offset,
    [property: JsonPropertyName("size")] long Size,
    [property: JsonPropertyName("sha256")] string Sha256,
    [property: JsonPropertyName("role")] string? Role,
    [property: JsonPropertyName("storedAs")] string StoredAs);

public sealed record FlashKitManifest(
    [property: JsonPropertyName("chip")] string Chip,
    [property: JsonPropertyName("flashMode")] string FlashMode,
    [property: JsonPropertyName("flashFreq")] string FlashFreq,
    [property: JsonPropertyName("flashSize")] string FlashSize,
    [property: JsonPropertyName("files")] IReadOnlyList<FlashKitFile> Files);

public sealed class FlashKitException(string message) : Exception(message);

public static class FlashKit
{
    public const string ManifestFileName = "manifest.json";
    public const long MaxEntryBytes = 16 * 1024 * 1024;
    public const long MaxTotalBytes = 32 * 1024 * 1024;
    private const int MaxEntries = 64;
    private const byte EspImageMagic = 0xE9;

    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };

    // Extracts the kit into kitDir and writes the app image to appPath.
    // Throws FlashKitException (caller maps to 400) for anything malformed.
    // Never uses zip entry names as filesystem paths: files are stored as
    // f<index>.bin, so a hostile entry name can't escape kitDir (zip-slip).
    public static async Task<(FlashKitManifest Manifest, string AppSha256, long AppSize)> ExtractAsync(
        Stream zipStream, string kitDir, string appPath)
    {
        using var zip = OpenZip(zipStream);
        if (zip.Entries.Count > MaxEntries)
            throw new FlashKitException($"Flash kit has too many entries (max {MaxEntries}).");

        long total = 0;
        foreach (var entry in zip.Entries)
        {
            if (!IsSafeEntryName(entry.FullName))
                throw new FlashKitException($"Unsafe path in flash kit: '{entry.FullName}'.");
            if (entry.Length > MaxEntryBytes)
                throw new FlashKitException($"'{entry.FullName}' is too large.");
            total += entry.Length;
        }
        if (total > MaxTotalBytes)
            throw new FlashKitException("Flash kit is too large when extracted.");

        var argsEntry = zip.GetEntry("flasher_args.json")
            ?? throw new FlashKitException("Flash kit is missing flasher_args.json at its root.");
        var args = await ReadFlasherArgsAsync(argsEntry);

        Directory.CreateDirectory(kitDir);
        var files = new List<FlashKitFile>();
        string? appSha = null;
        long appSize = 0;
        var index = 0;
        foreach (var (offset, path) in args.Files.OrderBy(f => f.Offset))
        {
            var entry = zip.GetEntry(path)
                ?? throw new FlashKitException($"flasher_args.json references '{path}', which is not in the kit.");
            var storedAs = $"f{index++}.bin";
            var target = Path.Combine(kitDir, storedAs);
            var (sha, size, firstByte) = await CopyAndHashAsync(entry, target);
            var role = args.Roles.GetValueOrDefault(path);

            if (role == "app")
            {
                if (firstByte != EspImageMagic)
                    throw new FlashKitException("App image is not an ESP32 image (missing 0xE9 magic byte).");
                File.Copy(target, appPath, overwrite: true);
                appSha = sha;
                appSize = size;
            }
            files.Add(new FlashKitFile(path, offset, size, sha, role, storedAs));
        }

        if (appSha is null)
            throw new FlashKitException("flasher_args.json has no \"app\" entry.");

        var manifest = new FlashKitManifest(args.Chip, args.FlashMode, args.FlashFreq, args.FlashSize, files);
        await File.WriteAllTextAsync(Path.Combine(kitDir, ManifestFileName), JsonSerializer.Serialize(manifest, JsonOptions));
        return (manifest, appSha, appSize);
    }

    public static async Task<FlashKitManifest?> ReadManifestAsync(string kitDir)
    {
        var path = Path.Combine(kitDir, ManifestFileName);
        if (!File.Exists(path)) return null;
        await using var stream = File.OpenRead(path);
        return await JsonSerializer.DeserializeAsync<FlashKitManifest>(stream);
    }

    private static ZipArchive OpenZip(Stream stream)
    {
        try
        {
            return new ZipArchive(stream, ZipArchiveMode.Read, leaveOpen: true);
        }
        catch (InvalidDataException)
        {
            throw new FlashKitException("File is not a valid zip archive.");
        }
    }

    private static bool IsSafeEntryName(string name) =>
        name.Length > 0
        && !name.Contains('\\')
        && !name.StartsWith('/')
        && !name.Contains(':')
        && name.Split('/').All(part => part != ".." && part != ".");

    private static async Task<(string Sha256, long Size, int FirstByte)> CopyAndHashAsync(ZipArchiveEntry entry, string target)
    {
        await using var input = entry.Open();
        await using var output = File.Create(target);
        using var sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        var buffer = new byte[81920];
        long size = 0;
        var firstByte = -1;
        int read;
        while ((read = await input.ReadAsync(buffer)) > 0)
        {
            if (size == 0) firstByte = buffer[0];
            size += read;
            if (size > MaxEntryBytes)
                throw new FlashKitException($"'{entry.FullName}' is too large.");
            sha.AppendData(buffer, 0, read);
            await output.WriteAsync(buffer.AsMemory(0, read));
        }
        return (Convert.ToHexStringLower(sha.GetHashAndReset()), size, firstByte);
    }

    private sealed record FlasherArgs(
        string Chip, string FlashMode, string FlashFreq, string FlashSize,
        List<(long Offset, string Path)> Files, Dictionary<string, string> Roles);

    private static async Task<FlasherArgs> ReadFlasherArgsAsync(ZipArchiveEntry entry)
    {
        JsonDocument doc;
        try
        {
            await using var stream = entry.Open();
            doc = await JsonDocument.ParseAsync(stream);
        }
        catch (JsonException)
        {
            throw new FlashKitException("flasher_args.json is not valid JSON.");
        }

        using (doc)
        {
            var root = doc.RootElement;
            if (!root.TryGetProperty("flash_files", out var flashFiles) || flashFiles.ValueKind != JsonValueKind.Object)
                throw new FlashKitException("flasher_args.json has no flash_files map.");

            var files = new List<(long, string)>();
            foreach (var prop in flashFiles.EnumerateObject())
            {
                var path = prop.Value.GetString();
                if (string.IsNullOrEmpty(path) || !IsSafeEntryName(path))
                    throw new FlashKitException($"Unsafe file path in flasher_args.json: '{path}'.");
                files.Add((ParseOffset(prop.Name), path));
            }

            // Top-level role entries look like "app": { "offset": "0x100000", "file": "synora.bin" }.
            var roles = new Dictionary<string, string>();
            foreach (var prop in root.EnumerateObject())
            {
                if (prop.Value.ValueKind == JsonValueKind.Object
                    && prop.Value.TryGetProperty("file", out var file)
                    && prop.Value.TryGetProperty("offset", out _)
                    && file.GetString() is { } filePath)
                {
                    roles[filePath] = prop.Name;
                }
            }
            if (!roles.ContainsValue("app"))
                throw new FlashKitException("flasher_args.json has no \"app\" entry.");

            var settings = root.TryGetProperty("flash_settings", out var s) ? s : default;
            string Setting(string key, string fallback) =>
                settings.ValueKind == JsonValueKind.Object && settings.TryGetProperty(key, out var v) && v.GetString() is { Length: > 0 } str
                    ? str
                    : fallback;
            var chip = root.TryGetProperty("extra_esptool_args", out var extra)
                && extra.TryGetProperty("chip", out var c) && c.GetString() is { Length: > 0 } chipName
                    ? chipName
                    : "esp32s3";

            return new FlasherArgs(chip, Setting("flash_mode", "dio"), Setting("flash_freq", "80m"),
                Setting("flash_size", "16MB"), files, roles);
        }
    }

    private static long ParseOffset(string text)
    {
        var trimmed = text.Trim();
        var isHex = trimmed.StartsWith("0x", StringComparison.OrdinalIgnoreCase);
        var ok = isHex
            ? long.TryParse(trimmed[2..], NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out var value)
            : long.TryParse(trimmed, NumberStyles.None, CultureInfo.InvariantCulture, out value);
        if (!ok || value < 0)
            throw new FlashKitException($"Invalid flash offset '{text}' in flasher_args.json.");
        return value;
    }
}
