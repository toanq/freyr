namespace Synora.Core;

public class User
{
    public Guid Id { get; set; }
    public required string Username { get; set; }
    public string? Email { get; set; }
    public byte[]? PasswordHash { get; set; }
    public byte[]? PasswordSalt { get; set; }
    public string? GoogleId { get; set; }
    public string? FacebookId { get; set; }
    public required UserRole Role { get; set; }
    public DateTimeOffset CreatedAt { get; set; }
    public string? FcmToken { get; set; }
    public string? AvatarPath { get; set; }
}
