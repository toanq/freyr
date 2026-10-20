using System.IO.Compression;
using System.Net;
using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;
using Microsoft.EntityFrameworkCore;
using Synora.Core;
using Xunit;

namespace Synora.Api.Tests;

// Flash-kit zip uploads (scripts/package-firmware.sh output) and the
// admin-only manifest/file endpoints the web console's USB flasher uses.
public partial class FirmwareEndpointTests
{
    private const string FlasherArgsJson = """
        {
          "flash_settings": { "flash_mode": "dio", "flash_size": "16MB", "flash_freq": "80m" },
          "flash_files": {
            "0x0": "bootloader/bootloader.bin",
            "0x8000": "partition_table/partition-table.bin",
            "0xd000": "ota_data_initial.bin",
            "0x100000": "synora.bin"
          },
          "bootloader": { "offset": "0x0", "file": "bootloader/bootloader.bin", "encrypted": "false" },
          "partition-table": { "offset": "0x8000", "file": "partition_table/partition-table.bin", "encrypted": "false" },
          "otadata": { "offset": "0xd000", "file": "ota_data_initial.bin", "encrypted": "false" },
          "app": { "offset": "0x100000", "file": "synora.bin", "encrypted": "false" },
          "extra_esptool_args": { "chip": "esp32s3" }
        }
        """;

    private static readonly byte[] KitApp = [0xE9, 7, 7, 7];

    private static Dictionary<string, byte[]> GoodKitEntries() => new()
    {
        ["flasher_args.json"] = Encoding.UTF8.GetBytes(FlasherArgsJson),
        ["bootloader/bootloader.bin"] = [0xE9, 1],
        ["partition_table/partition-table.bin"] = [0xAA, 0x50, 1],
        ["ota_data_initial.bin"] = [0xFF, 0xFF],
        ["synora.bin"] = KitApp,
    };

    private static MultipartFormDataContent KitUpload(string version, Dictionary<string, byte[]> entries)
    {
        using var buffer = new MemoryStream();
        using (var zip = new ZipArchive(buffer, ZipArchiveMode.Create, leaveOpen: true))
        {
            foreach (var (name, bytes) in entries)
            {
                using var stream = zip.CreateEntry(name).Open();
                stream.Write(bytes);
            }
        }
        var file = new ByteArrayContent(buffer.ToArray());
        file.Headers.ContentType = new MediaTypeHeaderValue("application/zip");
        return new MultipartFormDataContent
        {
            { new StringContent(version), "version" },
            { file, "file", "synora-1.0.0-flashkit.zip" },
        };
    }

    [Fact]
    public async Task UploadFlashKit_AsAdmin_StoresAppAndServesManifestAndFiles()
    {
        var storageDir = CreateTempDir();
        var client = CreateClient(storageDir, out var dbName);
        client.DefaultRequestHeaders.Authorization = BearerFor(Guid.NewGuid(), UserRole.Admin);

        var response = await client.PostAsync("/admin/firmware", KitUpload("2.0.0", GoodKitEntries()));

        var raw = await response.Content.ReadAsStringAsync();
        Assert.True(response.StatusCode == HttpStatusCode.OK, $"status={response.StatusCode} body={raw}");
        using var body = JsonDocument.Parse(raw);
        var id = body.RootElement.GetProperty("id").GetGuid();
        Assert.True(body.RootElement.GetProperty("hasFlashKit").GetBoolean());
        Assert.Equal(KitApp.Length, body.RootElement.GetProperty("sizeBytes").GetInt64());
        // The kit's app image is what OTA serves, unchanged.
        Assert.Equal(KitApp, await File.ReadAllBytesAsync(Path.Combine(storageDir, $"{id}.bin")));

        using var manifest = JsonDocument.Parse(await client.GetStringAsync($"/admin/firmware/{id}/flash-manifest"));
        Assert.Equal("esp32s3", manifest.RootElement.GetProperty("chip").GetString());
        Assert.Equal("dio", manifest.RootElement.GetProperty("flashMode").GetString());
        var files = manifest.RootElement.GetProperty("files").EnumerateArray().ToList();
        Assert.Equal(4, files.Count);
        var app = files.Single(f => f.GetProperty("role").GetString() == "app");
        Assert.Equal(0x100000, app.GetProperty("offset").GetInt64());
        Assert.Equal("synora.bin", app.GetProperty("name").GetString());

        var bootloader = await client.GetByteArrayAsync($"/admin/firmware/{id}/flash-files/bootloader/bootloader.bin");
        Assert.Equal(new byte[] { 0xE9, 1 }, bootloader);
        var unlisted = await client.GetAsync($"/admin/firmware/{id}/flash-files/manifest.json");
        Assert.Equal(HttpStatusCode.NotFound, unlisted.StatusCode);
    }

    [Fact]
    public async Task UploadFlashKit_WithoutAppEntry_ReturnsBadRequestAndLeavesNothing()
    {
        var storageDir = CreateTempDir();
        var client = CreateClient(storageDir, out var dbName);
        client.DefaultRequestHeaders.Authorization = BearerFor(Guid.NewGuid(), UserRole.Admin);
        var entries = GoodKitEntries();
        entries["flasher_args.json"] = Encoding.UTF8.GetBytes(
            """{ "flash_files": { "0x0": "bootloader/bootloader.bin" }, "bootloader": { "offset": "0x0", "file": "bootloader/bootloader.bin" } }""");

        var response = await client.PostAsync("/admin/firmware", KitUpload("2.0.1", entries));

        Assert.Equal(HttpStatusCode.BadRequest, response.StatusCode);
        Assert.Contains("app", await response.Content.ReadAsStringAsync());
        Assert.Empty(Directory.GetFileSystemEntries(storageDir));
        using var db = OpenDb(dbName);
        Assert.False(await db.FirmwareBuilds.AnyAsync());
    }

    [Fact]
    public async Task UploadFlashKit_WithZipSlipEntry_ReturnsBadRequest()
    {
        var storageDir = CreateTempDir();
        var client = CreateClient(storageDir, out _);
        client.DefaultRequestHeaders.Authorization = BearerFor(Guid.NewGuid(), UserRole.Admin);
        var entries = GoodKitEntries();
        entries["../../evil.bin"] = [0xE9];

        var response = await client.PostAsync("/admin/firmware", KitUpload("2.0.2", entries));

        Assert.Equal(HttpStatusCode.BadRequest, response.StatusCode);
        Assert.Contains("Unsafe path", await response.Content.ReadAsStringAsync());
        Assert.Empty(Directory.GetFileSystemEntries(storageDir));
        Assert.False(File.Exists(Path.Combine(storageDir, "..", "..", "evil.bin")));
    }

    [Fact]
    public async Task FlashManifest_AsUser_ReturnsForbidden()
    {
        var client = CreateClient(CreateTempDir(), out _);
        client.DefaultRequestHeaders.Authorization = BearerFor(Guid.NewGuid(), UserRole.Admin);
        var upload = await client.PostAsync("/admin/firmware", KitUpload("2.0.3", GoodKitEntries()));
        using var body = JsonDocument.Parse(await upload.Content.ReadAsStringAsync());
        var id = body.RootElement.GetProperty("id").GetGuid();

        client.DefaultRequestHeaders.Authorization = BearerFor(Guid.NewGuid(), UserRole.User);
        var manifest = await client.GetAsync($"/admin/firmware/{id}/flash-manifest");
        var file = await client.GetAsync($"/admin/firmware/{id}/flash-files/synora.bin");

        Assert.Equal(HttpStatusCode.Forbidden, manifest.StatusCode);
        Assert.Equal(HttpStatusCode.Forbidden, file.StatusCode);
    }

    [Fact]
    public async Task DeleteFirmware_WithFlashKit_RemovesKitDirectory()
    {
        var storageDir = CreateTempDir();
        var client = CreateClient(storageDir, out _);
        client.DefaultRequestHeaders.Authorization = BearerFor(Guid.NewGuid(), UserRole.Admin);
        var upload = await client.PostAsync("/admin/firmware", KitUpload("2.0.4", GoodKitEntries()));
        using var body = JsonDocument.Parse(await upload.Content.ReadAsStringAsync());
        var id = body.RootElement.GetProperty("id").GetGuid();
        Assert.True(Directory.Exists(Path.Combine(storageDir, id.ToString())));

        var response = await client.DeleteAsync($"/admin/firmware/{id}");

        Assert.Equal(HttpStatusCode.NoContent, response.StatusCode);
        Assert.Empty(Directory.GetFileSystemEntries(storageDir));
    }
}
