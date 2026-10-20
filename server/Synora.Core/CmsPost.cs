namespace Synora.Core;

// A landing-site news/blog post (synora-landing.carcise.dev), managed via
// the /cms/admin/posts endpoints and served publicly at /cms/posts. Ported
// from the former standalone Rust CMS (landing/server) -- field set and
// semantics match its `posts` table so the landing frontend's JSON contract
// is unchanged. Tags stay a single comma-separated string (the frontend
// splits it), not a normalized table.
public class CmsPost
{
    public Guid Id { get; set; }
    public required string Slug { get; set; }
    public required string Title { get; set; }
    public string Excerpt { get; set; } = "";
    public string Content { get; set; } = "";
    public string? CoverImage { get; set; }
    public string? Category { get; set; }
    public string Tags { get; set; } = "";
    public bool Published { get; set; }

    // Set the first time a post is saved as published, kept across later
    // edits while it stays published, cleared when unpublished.
    public DateTimeOffset? PublishedAt { get; set; }
    public DateTimeOffset CreatedAt { get; set; }
    public DateTimeOffset UpdatedAt { get; set; }
}
