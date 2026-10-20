namespace Synora.Core;

// A pre-order submitted from the landing site's public order form (POST
// /cms/orders), reviewed by admins at /cms/admin/orders. Ported from the
// former standalone Rust CMS's `orders` table.
public class CmsOrder
{
    public Guid Id { get; set; }
    public required string CustomerName { get; set; }
    public required string Phone { get; set; }
    public string? Email { get; set; }
    public string? Address { get; set; }
    public long Quantity { get; set; } = 1;
    public string? Note { get; set; }

    // One of CmsOrderStatuses.All ("new" on creation).
    public string Status { get; set; } = CmsOrderStatuses.New;
    public DateTimeOffset CreatedAt { get; set; }
}

public static class CmsOrderStatuses
{
    public const string New = "new";

    // Same set the landing admin UI offers (landing/src/pages/admin/AdminOrders.jsx).
    public static readonly IReadOnlySet<string> All =
        new HashSet<string> { New, "confirmed", "shipped", "done", "cancelled" };
}
