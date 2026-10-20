namespace Synora.Core;

// One FCM registration token per installation (phone app or browser), so a
// user's phone and web console can both receive pushes.
public class UserPushToken
{
    public Guid Id { get; set; }
    public Guid UserId { get; set; }
    public User? User { get; set; }
    public required string Token { get; set; }
    // "android" | "ios" | "web"
    public required string Platform { get; set; }
    public DateTimeOffset CreatedAt { get; set; }
    public DateTimeOffset UpdatedAt { get; set; }
}
