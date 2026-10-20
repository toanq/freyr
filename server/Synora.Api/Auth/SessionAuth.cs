using System.Security.Claims;
using Synora.Core;

namespace Synora.Api.Auth;

// Username/password check and cookie principal shape for the web-console
// login (POST /auth/login) -- shared with the landing CMS login (POST
// /cms/auth/login, see Cms/CmsEndpoints.cs) so both sign in exactly the same
// way. Program.cs's VerifyPassword/BuildPrincipal local functions delegate here.
public static class SessionAuth
{
    public static bool VerifyPassword(User? user, string password) =>
        user?.PasswordHash is not null && user.PasswordSalt is not null &&
        PasswordHasher.Verify(password, user.PasswordHash, user.PasswordSalt);

    public static ClaimsPrincipal BuildPrincipal(User user, string scheme) =>
        new(new ClaimsIdentity(
            [
                new Claim(ClaimTypes.NameIdentifier, user.Id.ToString()),
                new Claim(ClaimTypes.Name, user.Username),
                new Claim(ClaimTypes.Role, user.Role.ToString()),
            ],
            scheme));

    public static bool IsAdmin(ClaimsPrincipal principal) =>
        principal.IsInRole(nameof(UserRole.Admin)) || principal.IsInRole(nameof(UserRole.SuperAdmin));

    public static bool IsAdmin(UserRole role) => role is UserRole.Admin or UserRole.SuperAdmin;
}
