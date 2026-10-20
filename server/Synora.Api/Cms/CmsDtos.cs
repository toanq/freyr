using System.Text.Json.Serialization;
using Synora.Core;

namespace Synora.Api.Cms;

// JSON contract of the former Rust CMS (landing/server), which the landing
// frontend (landing/src/**) reads unchanged: snake_case field names, posts'
// `tags` as one comma-separated string, timestamps as ISO-8601 strings.
// Explicit [JsonPropertyName]s override the app-wide camelCase policy.

public sealed record CmsPostDto(
    [property: JsonPropertyName("id")] Guid Id,
    [property: JsonPropertyName("slug")] string Slug,
    [property: JsonPropertyName("title")] string Title,
    [property: JsonPropertyName("excerpt")] string Excerpt,
    [property: JsonPropertyName("content")] string Content,
    [property: JsonPropertyName("cover_image")] string? CoverImage,
    [property: JsonPropertyName("category")] string? Category,
    [property: JsonPropertyName("tags")] string Tags,
    [property: JsonPropertyName("published")] bool Published,
    [property: JsonPropertyName("published_at")] DateTimeOffset? PublishedAt,
    [property: JsonPropertyName("created_at")] DateTimeOffset CreatedAt,
    [property: JsonPropertyName("updated_at")] DateTimeOffset UpdatedAt)
{
    public static CmsPostDto From(CmsPost p) => new(
        p.Id, p.Slug, p.Title, p.Excerpt, p.Content, p.CoverImage, p.Category, p.Tags,
        p.Published, p.PublishedAt, p.CreatedAt, p.UpdatedAt);
}

public sealed record CmsOrderDto(
    [property: JsonPropertyName("id")] Guid Id,
    [property: JsonPropertyName("customer_name")] string CustomerName,
    [property: JsonPropertyName("phone")] string Phone,
    [property: JsonPropertyName("email")] string? Email,
    [property: JsonPropertyName("address")] string? Address,
    [property: JsonPropertyName("quantity")] long Quantity,
    [property: JsonPropertyName("note")] string? Note,
    [property: JsonPropertyName("status")] string Status,
    [property: JsonPropertyName("created_at")] DateTimeOffset CreatedAt)
{
    public static CmsOrderDto From(CmsOrder o) => new(
        o.Id, o.CustomerName, o.Phone, o.Email, o.Address, o.Quantity, o.Note, o.Status, o.CreatedAt);
}

// Create and update share one shape (Rust's NewPost/UpdatePost were identical).
// The admin editor PUTs the whole post back, so unknown fields (id,
// created_at, ...) are expected and ignored.
public sealed class CmsPostRequest
{
    [JsonPropertyName("slug")] public string? Slug { get; set; }
    [JsonPropertyName("title")] public string? Title { get; set; }
    [JsonPropertyName("excerpt")] public string? Excerpt { get; set; }
    [JsonPropertyName("content")] public string? Content { get; set; }
    [JsonPropertyName("cover_image")] public string? CoverImage { get; set; }
    [JsonPropertyName("category")] public string? Category { get; set; }
    [JsonPropertyName("tags")] public string? Tags { get; set; }
    [JsonPropertyName("published")] public bool Published { get; set; }
}

public sealed class CmsOrderRequest
{
    [JsonPropertyName("customer_name")] public string? CustomerName { get; set; }
    [JsonPropertyName("phone")] public string? Phone { get; set; }
    [JsonPropertyName("email")] public string? Email { get; set; }
    [JsonPropertyName("address")] public string? Address { get; set; }
    [JsonPropertyName("quantity")] public long Quantity { get; set; } = 1;
    [JsonPropertyName("note")] public string? Note { get; set; }
}

public sealed class CmsOrderStatusRequest
{
    [JsonPropertyName("status")] public string? Status { get; set; }
}

public sealed class CmsLoginRequest
{
    [JsonPropertyName("username")] public string? Username { get; set; }
    [JsonPropertyName("password")] public string? Password { get; set; }
}

public sealed record CmsLoginResponse([property: JsonPropertyName("username")] string Username);

// Rust returned { admin_id }; username is extra (the frontend only checks 2xx).
public sealed record CmsMeResponse(
    [property: JsonPropertyName("admin_id")] string AdminId,
    [property: JsonPropertyName("username")] string? Username);

public sealed record CmsUploadResponse([property: JsonPropertyName("url")] string Url);
