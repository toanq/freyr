using System.IdentityModel.Tokens.Jwt;
using System.Net.Http.Json;
using System.Security.Claims;
using System.Security.Cryptography;
using System.Text.Json.Serialization;
using FirebaseAdmin;
using FirebaseAdmin.Messaging;
using Google.Apis.Auth;
using Google.Apis.Auth.OAuth2;
using Microsoft.AspNetCore.Authentication;
using Microsoft.AspNetCore.Authentication.Cookies;
using Microsoft.AspNetCore.Authentication.JwtBearer;
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.Mvc;
using Microsoft.EntityFrameworkCore;
using Microsoft.Extensions.Options;
using Microsoft.IdentityModel.Tokens;
using Microsoft.OpenApi.Models;
using Synora.Api.Auth;
using Synora.Api.Cms;
using Synora.Api.Firmware;
using Synora.Api.Mqtt;
using Synora.Api.Services;
using Synora.Core;
using Synora.Infrastructure;
using Synora.Voice;

const string JwtIssuer = "synora";
const string JwtAudience = "synora-mobile";

var builder = WebApplication.CreateBuilder(args);

builder.Services.ConfigureHttpJsonOptions(options =>
    options.SerializerOptions.Converters.Add(new JsonStringEnumConverter()));

// Public hostname, e.g. Domain:App = synora.id.vn (landing at /, console at
// /app/, API at /api). Set via SYNORA_DOMAIN in .env -> docker-compose ->
// Domain__App. Domain:Landing is optional, only for a separately hosted landing site.
var appDomain = NormalizeDomain(builder.Configuration["Domain:App"]);
var landingDomain = NormalizeDomain(builder.Configuration["Domain:Landing"]);

var corsOrigins = new[] { "http://localhost:5173" }
    .Concat(new[] { appDomain, landingDomain }.Where(d => d is not null).Select(d => $"https://{d}"))
    .ToArray();

builder.Services.AddCors(options =>
{
    options.AddDefaultPolicy(policy =>
        policy.WithOrigins(corsOrigins)
              .AllowAnyHeader()
              .AllowAnyMethod()
              .AllowCredentials());
});

builder.Services.AddDbContext<SynoraDbContext>(options =>
    options.UseNpgsql(builder.Configuration.GetConnectionString("Postgres")));

builder.Services.AddHttpClient();

builder.Services.AddEndpointsApiExplorer();
builder.Services.AddSwaggerGen(options =>
{
    options.SwaggerDoc("v1", new OpenApiInfo { Title = "Synora API", Version = "v1" });

    // Most endpoints take a JWT bearer (mobile) or the cookie (web console) --
    // only the bearer scheme is practical to exercise from Swagger UI, so
    // that's the one wired up here: click "Authorize" and paste a token from
    // POST /auth/token.
    var bearerScheme = new OpenApiSecurityScheme
    {
        Name = "Authorization",
        Type = SecuritySchemeType.Http,
        Scheme = "bearer",
        BearerFormat = "JWT",
        In = ParameterLocation.Header,
        Reference = new OpenApiReference { Type = ReferenceType.SecurityScheme, Id = "Bearer" },
    };
    options.AddSecurityDefinition("Bearer", bearerScheme);
    options.AddSecurityRequirement(new OpenApiSecurityRequirement { [bearerScheme] = Array.Empty<string>() });
});

var avatarStoragePath = builder.Configuration["Avatar:StoragePath"] ?? "/data/avatars";
Directory.CreateDirectory(avatarStoragePath);

// Same file-on-disk-plus-path-column convention as avatars above, for the
// user's raw captured audio (see LlmVoicePipeline/DiscussionMessage.UserAudioPath)
// -- kept in its own directory since these are WAV recordings, not images.
var recordingsStoragePath = builder.Configuration["Recordings:StoragePath"] ?? "/data/recordings";
Directory.CreateDirectory(recordingsStoragePath);

// Same convention again, for WAV files published by the firmware's
// diagnostic `mictest` console command (see MqttService/MicTestUpload) --
// kept in its own directory since these are hardware-diagnostic recordings,
// not conversation history.
var micTestStoragePath = builder.Configuration["MicTestUploads:StoragePath"] ?? "/data/mictest";
Directory.CreateDirectory(micTestStoragePath);

// Same convention again, for admin-uploaded firmware .bin files (see
// FirmwareBuild) served back to devices for both the WiFi-OTA and BLE-push
// fallback update paths.
var firmwareStoragePath = builder.Configuration["Firmware:StoragePath"] ?? "/data/firmware";
Directory.CreateDirectory(firmwareStoragePath);

var jwtSigningKey = builder.Configuration["Jwt:SigningKey"];
var jwtConfigured = !string.IsNullOrEmpty(jwtSigningKey);

builder.Services
    .AddAuthentication(options =>
    {
        // Results.Forbid() (used throughout for manual IsAdmin(http)/owner
        // checks) needs a default forbid scheme to resolve -- with two
        // schemes registered below and no default, it throws
        // InvalidOperationException instead of returning 403.
        options.DefaultForbidScheme = JwtBearerDefaults.AuthenticationScheme;
        options.DefaultChallengeScheme = JwtBearerDefaults.AuthenticationScheme;
    })
    .AddCookie(CookieAuthenticationDefaults.AuthenticationScheme, options =>
    {
        options.Cookie.Name = "synora_auth";
        options.ExpireTimeSpan = TimeSpan.FromDays(7);
        options.SlidingExpiration = true;
        options.Events.OnRedirectToLogin = context =>
        {
            context.Response.StatusCode = StatusCodes.Status401Unauthorized;
            return Task.CompletedTask;
        };
    })
    .AddJwtBearer(JwtBearerDefaults.AuthenticationScheme, options =>
    {
        options.TokenValidationParameters = new TokenValidationParameters
        {
            ValidateIssuer = true,
            ValidIssuer = JwtIssuer,
            ValidateAudience = true,
            ValidAudience = JwtAudience,
            ValidateLifetime = true,
            ValidateIssuerSigningKey = true,
            IssuerSigningKey = new SymmetricSecurityKey(
                System.Text.Encoding.UTF8.GetBytes(jwtConfigured ? jwtSigningKey! : new string('0', 32))),
        };
    });

var authSchemes = new[] { CookieAuthenticationDefaults.AuthenticationScheme, JwtBearerDefaults.AuthenticationScheme };

builder.Services.AddAuthorization(options =>
{
    options.DefaultPolicy = new AuthorizationPolicyBuilder(authSchemes)
        .RequireAuthenticatedUser()
        .Build();

    options.AddPolicy("SuperAdminOnly", policy => policy
        .AddAuthenticationSchemes(authSchemes)
        .RequireRole(nameof(UserRole.SuperAdmin)));
});

// Device voice channel (xiaozhi protocol over MQTT). LlmVoicePipeline is the
// real pipeline in every environment: the transcript comes from a real
// ISpeechToText call (ASR sidecar, PhoWhisper-small), the reply text from a
// real ILlmClient call, and the reply audio from a real ITextToSpeech call
// (TTS sidecar, facebook/mms-tts-vie). "Testing" swaps in
// FakeSpeechToText/FakeLlmClient/FakeTextToSpeech (canned values, no
// network) so the pipeline can be exercised without hitting a real
// ASR/LLM/TTS backend.
builder.Services.Configure<LlmOptions>(builder.Configuration.GetSection("Llm"));
builder.Services.Configure<AsrOptions>(builder.Configuration.GetSection("Asr"));
builder.Services.Configure<TtsOptions>(builder.Configuration.GetSection("Tts"));
builder.Services.Configure<VoiceRecordingOptions>(builder.Configuration.GetSection("Recordings"));
builder.Services.AddSingleton<IVoicePipeline, LlmVoicePipeline>();
if (builder.Environment.IsEnvironment("Testing"))
{
    builder.Services.AddSingleton<ILlmClient, FakeLlmClient>();
    builder.Services.AddSingleton<ISpeechToText, FakeSpeechToText>();
    builder.Services.AddSingleton<ITextToSpeech, FakeTextToSpeech>();
}
else
{
    builder.Services.AddSingleton<ILlmClient, DeepSeekLlmClient>();
    builder.Services.AddSingleton<ISpeechToText, PhoWhisperClient>();
    builder.Services.AddSingleton<ITextToSpeech, VieNeuTtsClient>();
}
builder.Services.AddSingleton<McpBridge>();
builder.Services.AddSingleton<VoiceMqttDispatcher>();

// Device<->server link (telemetry/unlink) and voice control/audio, all over
// MQTT (Mosquitto) instead of REST polling/WebSocket -- see MqttService's doc
// comment for the topic scheme and how the MqttService<->VoiceMqttDispatcher
// circular dependency is avoided.
builder.Services.Configure<MqttOptions>(builder.Configuration.GetSection("Mqtt"));
builder.Services.Configure<MicTestUploadOptions>(builder.Configuration.GetSection("MicTestUploads"));
builder.Services.AddSingleton<IPushSender, FirebasePushSender>();
builder.Services.AddSingleton<PushNotifier>();
builder.Services.AddSingleton<LowWaterAlertService>();
builder.Services.AddSingleton<MqttService>();
builder.Services.AddSingleton<IDeviceMqttPublisher>(sp => sp.GetRequiredService<MqttService>());
// Same MqttService instance, resolved as the other interface it implements --
// needed by /admin/voice-test/chat to build an ad-hoc VoiceSession for real
// MCP tool calls (see that endpoint's comment).
builder.Services.AddSingleton<IVoicePublisher>(sp => sp.GetRequiredService<MqttService>());
// The hosted service (the actual broker connect/subscribe loop) is skipped
// under "Testing" -- there's no Mosquitto to connect to in the test host, and
// VoiceMqttDispatcher (the message-handling logic that matters for tests) is
// exercised directly, without going through a real MQTT connection. Program
// endpoints that publish (e.g. /water, /muted) still resolve IDeviceMqttPublisher
// fine; MqttService.PublishAsync just no-ops with a logged warning when
// disconnected.
if (!builder.Environment.IsEnvironment("Testing"))
{
    builder.Services.AddHostedService<MqttService>(sp => sp.GetRequiredService<MqttService>());
    builder.Services.AddHostedService<TimeSyncBroadcastService>();
}

var app = builder.Build();

var dashboardUsername = app.Configuration["Dashboard:Username"];
var dashboardPassword = app.Configuration["Dashboard:Password"];
var googleClientId = app.Configuration["OAuth:Google:ClientId"];
// Google Identity Services on the web issues ID tokens for a separate *web*
// OAuth client, so both client ids are accepted as the token audience.
var googleWebClientId = app.Configuration["OAuth:Google:WebClientId"];
var googleAudiences = new[] { googleClientId, googleWebClientId }
    .Where(id => !string.IsNullOrWhiteSpace(id))
    .Select(id => id!.Trim())
    .ToArray();
var facebookAppId = app.Configuration["OAuth:Facebook:AppId"];
var facebookAppSecret = app.Configuration["OAuth:Facebook:AppSecret"];
var facebookConfigured = !string.IsNullOrEmpty(facebookAppId) && !string.IsNullOrEmpty(facebookAppSecret);
var firebaseServiceAccountJson = app.Configuration["Firebase:ServiceAccountJson"];
var firebaseConfigured = !string.IsNullOrEmpty(firebaseServiceAccountJson);
// Public URL devices reach this API at (e.g. https://synora.id.vn/api) -- the
// request's own Scheme/Host is wrong behind Cloudflare+Traefik (http, /api stripped).
// Explicit Firmware:PublicBaseUrl wins; otherwise derived from Domain:App.
var firmwarePublicBaseUrl = app.Configuration["Firmware:PublicBaseUrl"]?.Trim().TrimEnd('/') is { Length: > 0 } explicitFirmwareUrl
    ? explicitFirmwareUrl
    : appDomain is not null ? $"https://{appDomain}/api" : null;

if (!jwtConfigured)
{
    app.Logger.LogWarning(
        "Mobile login is not configured — set Jwt__SigningKey (env var) or Jwt:SigningKey (appsettings) " +
        "to enable /auth/register, /auth/token and OAuth login.");
}

if (googleAudiences.Length == 0)
{
    app.Logger.LogWarning(
        "Google login is not configured — set OAuth__Google__ClientId (mobile) and/or " +
        "OAuth__Google__WebClientId (web console) to enable it.");
}

if (!facebookConfigured)
{
    app.Logger.LogWarning("Facebook login is not configured — set OAuth__Facebook__AppId and OAuth__Facebook__AppSecret to enable it.");
}

if (firebaseConfigured)
{
    FirebaseApp.Create(new AppOptions
    {
        Credential = CredentialFactory.FromJson<ServiceAccountCredential>(firebaseServiceAccountJson).ToGoogleCredential(),
    });
}
else
{
    app.Logger.LogWarning("Push notifications are not configured — set Firebase__ServiceAccountJson to enable them.");
}

if (!app.Environment.IsEnvironment("Testing"))
{
    using var scope = app.Services.CreateScope();
    var db = scope.ServiceProvider.GetRequiredService<SynoraDbContext>();
    db.Database.Migrate();

    if (!db.Users.Any())
    {
        if (!string.IsNullOrEmpty(dashboardUsername) && !string.IsNullOrEmpty(dashboardPassword))
        {
            var (hash, salt) = PasswordHasher.Hash(dashboardPassword);
            db.Users.Add(new User
            {
                Username = dashboardUsername,
                PasswordHash = hash,
                PasswordSalt = salt,
                Role = UserRole.SuperAdmin,
                CreatedAt = DateTimeOffset.UtcNow,
            });
            db.SaveChanges();
            app.Logger.LogInformation(
                "Seeded initial SuperAdmin '{Username}' from Dashboard__Username/Dashboard__Password.", dashboardUsername);
        }
        else
        {
            app.Logger.LogWarning(
                "No users exist and Dashboard__Username/Dashboard__Password are not set — " +
                "set them to seed the first SuperAdmin.");
        }
    }
}

app.UseCors();
app.UseAuthentication();
app.UseAuthorization();

// Cloudflare (in front of the public domain) caches responses under
// /swagger/* by file extension (.js/.css) for hours by default, since
// Swashbuckle serves them with no Cache-Control header of its own --
// but their content is regenerated from SwaggerUIOptions on every deploy,
// so a stale cached copy silently keeps serving pre-fix config/URLs long
// after a redeploy (confirmed live: CF-Cache-Status: HIT, Age: 5140s on
// index.js after the SwaggerEndpoint URL had already been fixed). no-store
// tells Cloudflare (and browsers) not to cache these at all.
app.Use(async (context, next) =>
{
    if (context.Request.Path.StartsWithSegments("/swagger"))
    {
        context.Response.Headers.CacheControl = "no-store";
    }
    await next();
});

app.UseSwagger();
// Relative, not "/swagger/v1/swagger.json" -- Traefik strips the public
// "/api" prefix before forwarding here (traefik/dynamic.yml's api-stripprefix
// middleware), so this app has no idea it's actually served under /api.
// An absolute URL resolves the browser's fetch to /swagger/v1/swagger.json
// (no /api prefix), which Traefik's PathPrefix("/") rule then routes to the
// web SPA instead of this API, returning index.html -- which Swagger UI then
// fails to parse as a spec ("does not specify a valid version field").
// A relative URL resolves against the current page's own URL instead,
// preserving whatever prefix got it there in the first place.
app.UseSwaggerUI(options => options.SwaggerEndpoint("v1/swagger.json", "Synora API v1"));

app.MapGet("/health", () => Results.Ok(new { status = "ok" }));

// The device voice channel (xiaozhi protocol) and the device-trust-model
// telemetry/unlink endpoints now live entirely on MQTT (see MqttService,
// VoiceMqttDispatcher) -- there's no more WebSocket /v1/ endpoint.

// --- Web console auth (cookie) ---

app.MapPost("/auth/login", async (HttpContext http, SynoraDbContext db, LoginRequest request) =>
{
    var user = await db.Users.FirstOrDefaultAsync(u => u.Username == request.Username);
    if (!VerifyPassword(user, request.Password))
    {
        return Results.Unauthorized();
    }

    await SessionAuth.SignInWebAsync(http, user!, persistent: request.RememberMe ?? true);
    return Results.Ok();
});

app.MapPost("/auth/logout", async (HttpContext http) =>
{
    await http.SignOutAsync(CookieAuthenticationDefaults.AuthenticationScheme);
    return Results.Ok();
});

app.MapGet("/auth/session", async (HttpContext http) =>
{
    var principal = (await http.AuthenticateAsync(CookieAuthenticationDefaults.AuthenticationScheme)) is { Succeeded: true } cookieResult
        ? cookieResult.Principal
        : (await http.AuthenticateAsync(JwtBearerDefaults.AuthenticationScheme)) is { Succeeded: true } jwtResult
            ? jwtResult.Principal
            : null;

    if (principal is null)
    {
        return Results.Ok(new { authenticated = false });
    }

    return Results.Ok(new
    {
        authenticated = true,
        username = principal.Identity?.Name,
        role = principal.FindFirstValue(ClaimTypes.Role),
    });
});

// --- Mobile auth (JWT): register, username/password, OAuth ---
// `?cookie=true` (web console) signs the browser in with the same cookie as
// /auth/login and returns 200 with no body, instead of issuing a JWT -- so
// those calls don't need Jwt:SigningKey configured.

var authGroup = app.MapGroup("/auth");

authGroup.MapPost("/register", async (HttpContext http, SynoraDbContext db, RegisterRequest request, bool cookie = false) =>
{
    if (!cookie && !jwtConfigured)
    {
        return NotConfigured("Registration");
    }

    if (string.IsNullOrWhiteSpace(request.Username) || string.IsNullOrEmpty(request.Password))
    {
        return Results.BadRequest("Username and password are required.");
    }

    if (await db.Users.AnyAsync(u => u.Username == request.Username))
    {
        return Results.Conflict("Username already exists.");
    }

    var (hash, salt) = PasswordHasher.Hash(request.Password);
    var user = new User
    {
        Username = request.Username,
        Email = request.Email,
        PasswordHash = hash,
        PasswordSalt = salt,
        Role = UserRole.User,
        CreatedAt = DateTimeOffset.UtcNow,
    };
    db.Users.Add(user);
    await db.SaveChangesAsync();

    return await IssueSessionAsync(http, user, cookie, persistent: request.RememberMe ?? true);
});

authGroup.MapPost("/token", async (SynoraDbContext db, LoginRequest request) =>
{
    if (!jwtConfigured)
    {
        return NotConfigured("Login");
    }

    var user = await db.Users.FirstOrDefaultAsync(u => u.Username == request.Username);
    if (!VerifyPassword(user, request.Password))
    {
        return Results.Unauthorized();
    }

    return Results.Ok(new { token = IssueJwt(user!, jwtSigningKey!) });
});

authGroup.MapPost("/oauth/google", async (HttpContext http, SynoraDbContext db, GoogleLoginRequest request, bool cookie = false) =>
{
    if ((!cookie && !jwtConfigured) || googleAudiences.Length == 0)
    {
        return NotConfigured("Google login");
    }

    GoogleJsonWebSignature.Payload payload;
    try
    {
        payload = await GoogleJsonWebSignature.ValidateAsync(request.IdToken, new GoogleJsonWebSignature.ValidationSettings
        {
            Audience = googleAudiences,
        });
    }
    catch (InvalidJwtException)
    {
        return Results.Unauthorized();
    }

    var user = await db.Users.FirstOrDefaultAsync(u => u.GoogleId == payload.Subject);
    if (user is null)
    {
        user = new User
        {
            Username = payload.Email,
            Email = payload.Email,
            GoogleId = payload.Subject,
            Role = UserRole.User,
            CreatedAt = DateTimeOffset.UtcNow,
        };
        db.Users.Add(user);
        await db.SaveChangesAsync();
    }

    return await IssueSessionAsync(http, user, cookie, persistent: true);
});

authGroup.MapPost("/oauth/facebook", async (HttpContext http, SynoraDbContext db, IHttpClientFactory httpClientFactory, FacebookLoginRequest request, bool cookie = false) =>
{
    if ((!cookie && !jwtConfigured) || !facebookConfigured)
    {
        return NotConfigured("Facebook login");
    }

    var client = httpClientFactory.CreateClient();

    var debugResponse = await client.GetAsync(
        $"https://graph.facebook.com/debug_token?input_token={Uri.EscapeDataString(request.AccessToken)}" +
        $"&access_token={Uri.EscapeDataString(facebookAppId!)}|{Uri.EscapeDataString(facebookAppSecret!)}");
    var debug = debugResponse.IsSuccessStatusCode
        ? await debugResponse.Content.ReadFromJsonAsync<FacebookDebugTokenResponse>()
        : null;

    if (debug?.Data is not { IsValid: true } data || data.AppId != facebookAppId)
    {
        return Results.Unauthorized();
    }

    var profileResponse = await client.GetAsync(
        $"https://graph.facebook.com/me?fields=id,email&access_token={Uri.EscapeDataString(request.AccessToken)}");
    var profile = profileResponse.IsSuccessStatusCode
        ? await profileResponse.Content.ReadFromJsonAsync<FacebookProfile>()
        : null;
    if (profile is null)
    {
        return Results.Unauthorized();
    }

    var user = await db.Users.FirstOrDefaultAsync(u => u.FacebookId == profile.Id);
    if (user is null)
    {
        user = new User
        {
            Username = profile.Email ?? $"fb_{profile.Id}",
            Email = profile.Email,
            FacebookId = profile.Id,
            Role = UserRole.User,
            CreatedAt = DateTimeOffset.UtcNow,
        };
        db.Users.Add(user);
        await db.SaveChangesAsync();
    }

    return await IssueSessionAsync(http, user, cookie, persistent: true);
});

async Task<IResult> IssueSessionAsync(HttpContext http, User user, bool cookie, bool persistent)
{
    if (!cookie)
    {
        return Results.Ok(new { token = IssueJwt(user, jwtSigningKey!) });
    }
    await SessionAuth.SignInWebAsync(http, user, persistent);
    return Results.Ok();
}

// --- User management (SuperAdmin only) ---

var usersGroup = app.MapGroup("/users").RequireAuthorization("SuperAdminOnly");

usersGroup.MapGet("/", async (SynoraDbContext db) =>
    await db.Users
        .OrderBy(u => u.Username)
        .Select(u => new UserSummaryDto(u.Id, u.Username, u.Email, u.Role.ToString(), u.CreatedAt, db.PushTokens.Any(t => t.UserId == u.Id)))
        .ToListAsync());

usersGroup.MapPost("/", async (SynoraDbContext db, CreateUserRequest request) =>
{
    if (request.Role == UserRole.SuperAdmin)
    {
        return Results.BadRequest("Cannot create another SuperAdmin.");
    }

    if (await db.Users.AnyAsync(u => u.Username == request.Username))
    {
        return Results.Conflict("Username already exists.");
    }

    var (hash, salt) = PasswordHasher.Hash(request.Password);
    var user = new User
    {
        Username = request.Username,
        PasswordHash = hash,
        PasswordSalt = salt,
        Role = request.Role,
        CreatedAt = DateTimeOffset.UtcNow,
    };
    db.Users.Add(user);
    await db.SaveChangesAsync();

    return Results.Created($"/users/{user.Id}", new { user.Id, user.Username, Role = user.Role.ToString() });
});

usersGroup.MapPost("/{id:guid}/reset-password", async (Guid id, SynoraDbContext db, ResetPasswordRequest request) =>
{
    var user = await db.Users.FindAsync(id);
    if (user is null)
    {
        return Results.NotFound();
    }
    if (request.NewPassword.Length < 6)
    {
        return Results.BadRequest("Mật khẩu mới phải có ít nhất 6 ký tự.");
    }

    var (hash, salt) = PasswordHasher.Hash(request.NewPassword);
    user.PasswordHash = hash;
    user.PasswordSalt = salt;
    await db.SaveChangesAsync();
    return Results.Ok();
});

usersGroup.MapPost("/{id:guid}/notifications/test", async (Guid id, SynoraDbContext db, PushNotifier push, SendTestNotificationRequest request) =>
{
    if (!push.IsConfigured)
    {
        return NotConfigured("Push notifications");
    }

    var user = await db.Users.FindAsync(id);
    if (user is null)
    {
        return Results.NotFound();
    }
    if (!await db.PushTokens.AnyAsync(t => t.UserId == id))
    {
        return Results.BadRequest("This user has no registered device for push notifications.");
    }

    var result = await push.SendToUserAsync(db, id, new PushMessage(request.Title, request.Body, Link: push.AppLink("/")));
    await db.SaveChangesAsync();
    if (result.Sent == 0)
    {
        return Results.Problem(
            title: "Failed to send notification",
            detail: $"No registered device accepted it ({result.Removed} expired token(s) removed, {result.Failed} failed).",
            statusCode: StatusCodes.Status502BadGateway);
    }
    return Results.Ok(new { result.Sent, result.Removed, result.Failed });
});

// Any authenticated user (the mobile app) registers/updates its own push token.
app.MapPut("/users/me/fcm-token", async (SynoraDbContext db, HttpContext http, FcmTokenRequest request) =>
{
    var userId = CurrentUserId(http);
    if (await db.Users.FindAsync(userId) is null)
    {
        return Results.NotFound();
    }
    var token = request.Token?.Trim();
    if (string.IsNullOrEmpty(token) || token.Length > 4096)
    {
        return Results.BadRequest("Token is required.");
    }
    // The phone app sends only { token }; it predates the platform field.
    var platform = request.Platform?.Trim().ToLowerInvariant() switch
    {
        null or "" => "android",
        "android" or "ios" or "web" => request.Platform!.Trim().ToLowerInvariant(),
        _ => null,
    };
    if (platform is null)
    {
        return Results.BadRequest("Platform must be android, ios or web.");
    }

    var now = DateTimeOffset.UtcNow;
    // Unique per token: re-registering moves it to the current user (shared browser/phone).
    var existing = await db.PushTokens.FirstOrDefaultAsync(t => t.Token == token);
    if (existing is null)
    {
        db.PushTokens.Add(new UserPushToken
        {
            UserId = userId, Token = token, Platform = platform, CreatedAt = now, UpdatedAt = now,
        });
    }
    else
    {
        existing.UserId = userId;
        existing.Platform = platform;
        existing.UpdatedAt = now;
    }
    await db.SaveChangesAsync();
    return Results.Ok();
}).RequireAuthorization();

// Web opt-out / sign-out: forget this installation's token.
app.MapDelete("/users/me/fcm-token", async (SynoraDbContext db, HttpContext http, string token) =>
{
    var userId = CurrentUserId(http);
    var existing = await db.PushTokens.FirstOrDefaultAsync(t => t.Token == token && t.UserId == userId);
    if (existing is null)
    {
        return Results.NoContent();
    }
    db.PushTokens.Remove(existing);
    await db.SaveChangesAsync();
    return Results.NoContent();
}).RequireAuthorization();

app.MapGet("/users/me", async (SynoraDbContext db, HttpContext http) =>
{
    var user = await db.Users.FindAsync(CurrentUserId(http));
    if (user is null)
    {
        return Results.NotFound();
    }
    return Results.Ok(new
    {
        user.Id,
        user.Username,
        user.Email,
        Role = user.Role.ToString(),
        AvatarUrl = user.AvatarPath != null ? $"/users/{user.Id}/avatar" : null,
    });
}).RequireAuthorization();

app.MapPatch("/users/me", async (SynoraDbContext db, HttpContext http, UpdateProfileRequest request) =>
{
    var user = await db.Users.FindAsync(CurrentUserId(http));
    if (user is null)
    {
        return Results.NotFound();
    }
    user.Email = string.IsNullOrWhiteSpace(request.Email) ? null : request.Email.Trim();
    await db.SaveChangesAsync();
    return Results.Ok();
}).RequireAuthorization();

app.MapPatch("/users/me/password", async (SynoraDbContext db, HttpContext http, ChangePasswordRequest request) =>
{
    var user = await db.Users.FindAsync(CurrentUserId(http));
    if (user is null)
    {
        return Results.NotFound();
    }
    if (!VerifyPassword(user, request.CurrentPassword))
    {
        return Results.BadRequest("Mật khẩu hiện tại không đúng.");
    }
    if (request.NewPassword.Length < 6)
    {
        return Results.BadRequest("Mật khẩu mới phải có ít nhất 6 ký tự.");
    }

    var (hash, salt) = PasswordHasher.Hash(request.NewPassword);
    user.PasswordHash = hash;
    user.PasswordSalt = salt;
    await db.SaveChangesAsync();
    return Results.Ok();
}).RequireAuthorization();

app.MapPost("/users/me/avatar", async (SynoraDbContext db, HttpContext http, IFormFile file) =>
{
    var user = await db.Users.FindAsync(CurrentUserId(http));
    if (user is null)
    {
        return Results.NotFound();
    }

    var extensionByContentType = new Dictionary<string, string>
    {
        ["image/jpeg"] = ".jpg",
        ["image/png"] = ".png",
        ["image/webp"] = ".webp",
    };
    if (!extensionByContentType.TryGetValue(file.ContentType, out var ext))
    {
        return Results.BadRequest("Unsupported image type. Use JPEG, PNG, or WebP.");
    }

    const long maxBytes = 8 * 1024 * 1024;
    if (file.Length is 0 or > maxBytes)
    {
        return Results.BadRequest("Image must be between 1 byte and 8MB.");
    }

    if (user.AvatarPath is not null)
    {
        var oldPath = Path.Combine(avatarStoragePath, user.AvatarPath);
        if (File.Exists(oldPath))
        {
            File.Delete(oldPath);
        }
    }

    // "user-" prefix so filenames can never collide with device avatars, which
    // share the same storage directory and are just "{deviceId}{ext}".
    var fileName = $"user-{user.Id}{ext}";
    var fullPath = Path.Combine(avatarStoragePath, fileName);
    await using (var stream = File.Create(fullPath))
    {
        await file.CopyToAsync(stream);
    }

    user.AvatarPath = fileName;
    await db.SaveChangesAsync();

    return Results.Ok(new { avatarUrl = $"/users/{user.Id}/avatar" });
})
.DisableAntiforgery() // Same reasoning as the device avatar upload endpoint above.
.RequireAuthorization();

app.MapGet("/users/{id:guid}/avatar", async (Guid id, SynoraDbContext db) =>
{
    var user = await db.Users.FindAsync(id);
    if (user?.AvatarPath is null)
    {
        return Results.NotFound();
    }

    var fullPath = Path.Combine(avatarStoragePath, user.AvatarPath);
    if (!File.Exists(fullPath))
    {
        return Results.NotFound();
    }

    var contentType = Path.GetExtension(fullPath) switch
    {
        ".png" => "image/png",
        ".webp" => "image/webp",
        _ => "image/jpeg",
    };
    return Results.File(fullPath, contentType);
});

// --- Device pairing + ownership ---

var devicesGroup = app.MapGroup("/devices");

devicesGroup.MapPost("/activation", async (SynoraDbContext db, ActivateDeviceRequest request) =>
{
    string code;
    do
    {
        code = Random.Shared.Next(0, 1_000_000).ToString("D6");
    }
    while (await db.Devices.AnyAsync(d => d.ActivationCode == code));

    // Normalized (uppercase) before every compare/store below -- the
    // firmware's own POST here sends lowercase (GetMacAddressString()'s
    // snprintf format), while the app's /pair request below sends whatever
    // it read off the BLE deviceInfo characteristic uppercased. Comparing
    // raw strings let a device's own re-activation silently miss its
    // already-paired row (case mismatch) and create a brand-new,
    // never-actually-linked duplicate instead -- confirmed live, this
    // produced two rows for the same physical MAC within one test session.
    var normalizedMac = NormalizeMac(request.MacAddress);

    Device? device = null;
    if (!string.IsNullOrEmpty(normalizedMac))
    {
        device = await db.Devices.FirstOrDefaultAsync(d => d.MacAddress == normalizedMac);
    }

    if (device is not null)
    {
        // Reuse the same physical device's row instead of creating a
        // duplicate. This endpoint isn't only hit on first boot/factory
        // reset as originally assumed -- the device also re-activates any
        // time the app re-runs Wi-Fi provisioning (e.g. the BLE deviceInfo
        // "isPaired" flag being stale because telemetry hasn't posted
        // successfully yet), so unconditionally clearing OwnerUserId/PairedAt
        // here was silently unpairing devices that were still legitimately
        // owned. Only clear pairing state if it wasn't already paired --
        // an actual unpair (factory reset) already nulls OwnerUserId itself
        // before/via a separate call, so this stays a no-op for that case.
        device.ActivationCode = code;
        if (device.OwnerUserId is null)
        {
            device.PairedAt = null;
        }
    }
    else
    {
        device = new Device
        {
            Name = DeviceSerial(normalizedMac) is string serial ? $"Synora-{serial}" : "New device",
            ActivationCode = code,
            MacAddress = normalizedMac,
            CreatedAt = DateTimeOffset.UtcNow,
        };
        db.Devices.Add(device);
    }
    await db.SaveChangesAsync();

    return Results.Ok(new { deviceId = device.Id, activationCode = code });
});

static string? NormalizeMac(string? mac) => string.IsNullOrEmpty(mac) ? mac : mac.ToUpperInvariant();

// Matches the firmware's own Synora-XXXX BLE/AP naming (last 2 MAC bytes, 4
// hex chars) so the same physical device is identifiable the same way
// everywhere -- BLE scan list, AP name, and this default device name.
static string? DeviceSerial(string? macAddress)
{
    if (string.IsNullOrEmpty(macAddress))
    {
        return null;
    }
    var parts = macAddress.Split(':');
    return parts.Length >= 2 ? $"{parts[^2]}{parts[^1]}".ToUpperInvariant() : null;
}

devicesGroup.MapPost("/pair", async (SynoraDbContext db, HttpContext http, IDeviceMqttPublisher mqtt, PairDeviceRequest request) =>
{
    var device = await db.Devices.FirstOrDefaultAsync(d => d.ActivationCode == request.ActivationCode);
    if (device is null)
    {
        return Results.NotFound();
    }
    if (device.OwnerUserId is not null)
    {
        // Already paired to the same user asking again (e.g. a redundant
        // pairing attempt caused by the device's own stale isPaired cache)
        // is a no-op success, not a conflict -- only a genuinely different
        // owner should be rejected.
        if (device.OwnerUserId == CurrentUserId(http))
        {
            return Results.Ok(new { device.Id, device.Name, device.PairedAt });
        }
        return Results.Conflict("Device already paired.");
    }

    device.OwnerUserId = CurrentUserId(http);
    device.PairedAt = DateTimeOffset.UtcNow;
    // Defensive fallback -- normally already set by /devices/activation, but
    // this backfills it for rows activated before that existed, or if the
    // activation call's MAC somehow didn't make it through. Normalized the
    // same way as /devices/activation above -- otherwise this (app-sourced,
    // uppercased) write overwrites the device's own lowercase MAC and breaks
    // that endpoint's exact-match dedup lookup on the device's next
    // reactivation.
    var normalizedMac = NormalizeMac(request.MacAddress);
    if (!string.IsNullOrEmpty(normalizedMac))
    {
        device.MacAddress = normalizedMac;
    }
    await db.SaveChangesAsync();
    // isPaired flipped -- refresh the retained state topic so the device
    // picks it up without waiting for its next telemetry-driven anything.
    await mqtt.PublishStateAsync(device.Id);

    return Results.Ok(new { device.Id, device.Name, device.PairedAt });
}).RequireAuthorization();

devicesGroup.MapGet("/", async (SynoraDbContext db, HttpContext http) =>
{
    var isAdmin = http.User.IsInRole(nameof(UserRole.Admin)) || http.User.IsInRole(nameof(UserRole.SuperAdmin));
    var query = db.Devices.AsQueryable();
    if (!isAdmin)
    {
        var userId = CurrentUserId(http);
        query = query.Where(d => d.OwnerUserId == userId);
    }

    var devices = await query
        .OrderBy(d => d.Name)
        .Select(d => new
        {
            d.Id,
            d.Name,
            d.MacAddress,
            d.PairedAt,
            d.LastSeenAt,
            d.LowWaterWarningThresholdPercent,
            d.LowWaterCriticalThresholdPercent,
            d.LowWaterAlertIntervalMinutes,
            d.XiaozhiActivationCode,
            d.Muted,
            d.FirmwareVersion,
            AvatarUrl = d.AvatarPath != null ? $"/devices/{d.Id}/avatar" : null,
            Owner = d.Owner != null ? d.Owner.Username : null,
            LatestReading = db.SensorReadings
                .Where(r => r.DeviceId == d.Id)
                .OrderByDescending(r => r.RecordedAt)
                .Select(r => new SensorReadingDto(
                    r.TemperatureCelsius, r.HumidityPercent, r.SoilMoisturePercent, r.WaterLevelPercent, r.RecordedAt))
                .FirstOrDefault(),
        })
        .ToListAsync();

    // Computed in memory (not translatable as a clean single SQL projection)
    // -- was duplicated client-side logic in the Flutter app before, moved
    // here so both the app and any future client get the same suggestion
    // from one place. Projected into a named record (not an anonymous type)
    // -- Swashbuckle can't produce a valid $ref name for an anonymous type
    // wrapping another anonymous type (LatestReading), which broke Swagger
    // UI's rendering entirely ("Unable to render this definition").
    return devices.Select(d => new DeviceSummaryDto(
        d.Id,
        d.Name,
        d.MacAddress,
        d.PairedAt,
        d.LowWaterWarningThresholdPercent,
        d.LowWaterCriticalThresholdPercent,
        d.LowWaterAlertIntervalMinutes,
        d.AvatarUrl,
        d.Owner,
        d.LatestReading,
        ComputeCareSuggestion(
            d.LatestReading?.SoilMoisturePercent,
            d.LatestReading?.TemperatureCelsius,
            d.LatestReading?.HumidityPercent,
            d.LatestReading?.WaterLevelPercent),
        ComputeIsOnline(d.LastSeenAt),
        d.XiaozhiActivationCode,
        d.Muted,
        d.FirmwareVersion));
}).RequireAuthorization();

// Any device-trust-model request (telemetry/pending-command/schedules) counts
// as a heartbeat -- IsOnline is computed from LastSeenAt at query time (not
// persisted) so this threshold can be tuned without a migration.
static bool ComputeIsOnline(DateTimeOffset? lastSeenAt) =>
    lastSeenAt is not null && lastSeenAt > DateTimeOffset.UtcNow.AddSeconds(-60);

// Mirrors the thresholds/copy the Flutter app used to compute client-side
// (device_detail_screen.dart's _statusFor/"AI care suggestion" card) --
// same four metrics, same low/high bands, same three messages.
static string ComputeCareSuggestion(double? soil, double? temperature, double? humidity, double? water)
{
    static string StatusFor(double? value, double low, double high) =>
        value is null ? "—" : value < low ? "Thấp" : value > high ? "Cao" : "Tốt";

    var soilStatus = StatusFor(soil, 30, 60);
    var tempStatus = StatusFor(temperature, 18, 30);
    var humidityStatus = StatusFor(humidity, 40, 80);
    var waterStatus = StatusFor(water, 20, 100);

    if (soilStatus == "Thấp")
    {
        return "Đất khá khô, bạn nên tưới nước cho cây.";
    }
    if (new[] { soilStatus, tempStatus, humidityStatus, waterStatus }.All(s => s == "Tốt"))
    {
        return "Cây của bạn đang phát triển tốt, chưa cần chăm sóc thêm.";
    }
    return "Một vài chỉ số chưa lý tưởng, hãy theo dõi cây trong vài ngày tới.";
}

devicesGroup.MapGet("/{id:guid}", async (Guid id, SynoraDbContext db, HttpContext http) =>
{
    var isAdmin = http.User.IsInRole(nameof(UserRole.Admin)) || http.User.IsInRole(nameof(UserRole.SuperAdmin));
    var query = db.Devices.Where(d => d.Id == id);
    if (!isAdmin)
    {
        var userId = CurrentUserId(http);
        query = query.Where(d => d.OwnerUserId == userId);
    }

    var device = await query
        .Select(d => new
        {
            d.Id,
            d.Name,
            d.MacAddress,
            d.PairedAt,
            d.LastSeenAt,
            d.LowWaterWarningThresholdPercent,
            d.LowWaterCriticalThresholdPercent,
            d.LowWaterAlertIntervalMinutes,
            d.XiaozhiActivationCode,
            d.Muted,
            d.FirmwareVersion,
            AvatarUrl = d.AvatarPath != null ? $"/devices/{d.Id}/avatar" : null,
            Owner = d.Owner != null ? d.Owner.Username : null,
            LatestReading = db.SensorReadings
                .Where(r => r.DeviceId == d.Id)
                .OrderByDescending(r => r.RecordedAt)
                .Select(r => new SensorReadingDto(
                    r.TemperatureCelsius, r.HumidityPercent, r.SoilMoisturePercent, r.WaterLevelPercent, r.RecordedAt))
                .FirstOrDefault(),
        })
        .FirstOrDefaultAsync();

    if (device is null)
    {
        return Results.NotFound();
    }

    return Results.Ok(new DeviceSummaryDto(
        device.Id,
        device.Name,
        device.MacAddress,
        device.PairedAt,
        device.LowWaterWarningThresholdPercent,
        device.LowWaterCriticalThresholdPercent,
        device.LowWaterAlertIntervalMinutes,
        device.AvatarUrl,
        device.Owner,
        device.LatestReading,
        ComputeCareSuggestion(
            device.LatestReading?.SoilMoisturePercent,
            device.LatestReading?.TemperatureCelsius,
            device.LatestReading?.HumidityPercent,
            device.LatestReading?.WaterLevelPercent),
        ComputeIsOnline(device.LastSeenAt),
        device.XiaozhiActivationCode,
        device.Muted,
        device.FirmwareVersion));
}).RequireAuthorization();

// Most-recent-first page of this device's voice conversation history (see
// DiscussionMessage / LlmVoicePipeline). Named DTO, not the raw EF entity or
// an anonymous type -- same Swashbuckle $ref-naming reasoning as
// DeviceSummaryDto/SensorReadingDto above.
devicesGroup.MapGet("/{id:guid}/discussions", async (Guid id, SynoraDbContext db, HttpContext http) =>
{
    var isAdmin = http.User.IsInRole(nameof(UserRole.Admin)) || http.User.IsInRole(nameof(UserRole.SuperAdmin));
    var userId = CurrentUserId(http);
    var device = await db.Devices.FirstOrDefaultAsync(d => d.Id == id && (isAdmin || d.OwnerUserId == userId));
    if (device is null)
    {
        return Results.NotFound();
    }

    var discussions = await db.DiscussionMessages
        .Where(m => m.DeviceId == id)
        .OrderByDescending(m => m.CreatedAt)
        .Take(100)
        .Select(m => new DiscussionMessageDto(
            m.Id, m.SessionId, m.UserText, m.AssistantText, m.CreatedAt, m.UserAudioPath != null))
        .ToListAsync();

    return Results.Ok(discussions);
}).RequireAuthorization();

// Serves the WAV recording of what the user actually said for one turn, so
// the web console can play it back next to the transcribed UserText to spot
// STT errors -- see DiscussionMessage.UserAudioPath/LlmVoicePipeline. Same
// owner-or-admin gating as GET /devices/{id}/discussions above (this is
// scoped under the same device, not a bare message-id lookup, so the
// ownership check stays on the device row).
devicesGroup.MapGet("/{id:guid}/discussions/{messageId:guid}/audio", async (Guid id, Guid messageId, SynoraDbContext db, HttpContext http) =>
{
    var isAdmin = http.User.IsInRole(nameof(UserRole.Admin)) || http.User.IsInRole(nameof(UserRole.SuperAdmin));
    var userId = CurrentUserId(http);
    var device = await db.Devices.FirstOrDefaultAsync(d => d.Id == id && (isAdmin || d.OwnerUserId == userId));
    if (device is null)
    {
        return Results.NotFound();
    }

    var message = await db.DiscussionMessages.FirstOrDefaultAsync(m => m.Id == messageId && m.DeviceId == id);
    if (message?.UserAudioPath is null)
    {
        return Results.NotFound();
    }

    var fullPath = Path.Combine(recordingsStoragePath, message.UserAudioPath);
    if (!File.Exists(fullPath))
    {
        return Results.NotFound();
    }

    return Results.File(fullPath, "audio/wav");
}).RequireAuthorization();

// Admin-only: injects a fake reading through the exact same path as real
// device telemetry (including the low-water alert check) so alerts/UI can be
// tested without needing real hardware to physically reach a threshold. Real
// telemetry itself now arrives over MQTT (synora/devices/{id}/telemetry --
// see MqttService), not REST, but this admin utility is unrelated to the
// device-trust-model transport and stays REST.
devicesGroup.MapPost("/{id:guid}/test-reading", async (
    Guid id, SynoraDbContext db, HttpContext http, LowWaterAlertService lowWaterAlerts, TelemetryRequest request) =>
{
    var isAdmin = http.User.IsInRole(nameof(UserRole.Admin)) || http.User.IsInRole(nameof(UserRole.SuperAdmin));
    if (!isAdmin)
    {
        return Results.Forbid();
    }

    var device = await db.Devices.Include(d => d.Owner).FirstOrDefaultAsync(d => d.Id == id);
    if (device is null)
    {
        return Results.NotFound();
    }

    db.SensorReadings.Add(new SensorReading
    {
        DeviceId = id,
        TemperatureCelsius = request.TemperatureCelsius,
        HumidityPercent = request.HumidityPercent,
        SoilMoisturePercent = request.SoilMoisturePercent,
        WaterLevelPercent = request.WaterLevelPercent,
        RecordedAt = DateTimeOffset.UtcNow,
    });

    await lowWaterAlerts.CheckLowWaterAlertAsync(db, device, request.WaterLevelPercent);
    await db.SaveChangesAsync();
    return Results.Ok();
}).RequireAuthorization();

devicesGroup.MapPatch("/{id:guid}", async (Guid id, SynoraDbContext db, HttpContext http, RenameDeviceRequest request) =>
{
    var isAdmin = http.User.IsInRole(nameof(UserRole.Admin)) || http.User.IsInRole(nameof(UserRole.SuperAdmin));
    var userId = CurrentUserId(http);
    var device = await db.Devices.FirstOrDefaultAsync(d => d.Id == id && (isAdmin || d.OwnerUserId == userId));
    if (device is null)
    {
        return Results.NotFound();
    }

    device.Name = request.Name;
    await db.SaveChangesAsync();
    return Results.Ok();
}).RequireAuthorization();

devicesGroup.MapPatch("/{id:guid}/alerts", async (Guid id, SynoraDbContext db, HttpContext http, UpdateAlertsRequest request) =>
{
    var isAdmin = http.User.IsInRole(nameof(UserRole.Admin)) || http.User.IsInRole(nameof(UserRole.SuperAdmin));
    var userId = CurrentUserId(http);
    var device = await db.Devices.FirstOrDefaultAsync(d => d.Id == id && (isAdmin || d.OwnerUserId == userId));
    if (device is null)
    {
        return Results.NotFound();
    }
    // 0 is treated the same as null (disabled), per how the app's toggle UI
    // represents "off".
    var warning = request.LowWaterWarningThresholdPercent is 0 ? null : request.LowWaterWarningThresholdPercent;
    var critical = request.LowWaterCriticalThresholdPercent is 0 ? null : request.LowWaterCriticalThresholdPercent;

    if (warning is int w && (w < 1 || w > 100))
    {
        return Results.BadRequest("LowWaterWarningThresholdPercent must be between 1 and 100.");
    }
    if (critical is int c && (c < 1 || c > 100))
    {
        return Results.BadRequest("LowWaterCriticalThresholdPercent must be between 1 and 100.");
    }
    // Auto-raise the warning threshold to match critical, rather than
    // rejecting the request -- warning must never sit below critical (the
    // more severe, lower level), and adjusting for the caller is friendlier
    // than a validation error for what's a mechanical fixup.
    if (warning is int w2 && critical is int c2 && w2 < c2)
    {
        warning = critical;
    }
    if (request.LowWaterAlertIntervalMinutes is int minutes && minutes < 1)
    {
        return Results.BadRequest("LowWaterAlertIntervalMinutes must be at least 1.");
    }

    device.LowWaterWarningThresholdPercent = warning;
    device.LowWaterCriticalThresholdPercent = critical;
    device.LowWaterAlertIntervalMinutes = request.LowWaterAlertIntervalMinutes;
    // Re-arm whenever the setting changes, so e.g. raising the threshold
    // above the current (already-alerted) level can fire again immediately.
    device.LowWaterLastAlertedAt = null;
    await db.SaveChangesAsync();
    return Results.Ok();
}).RequireAuthorization();

// Despite the route/verb, this UNPAIRS rather than deletes the row -- a
// physical device that's ever activated should always stay visible
// somewhere in the system (to admins, via GET /devices' isAdmin branch
// returning every row regardless of owner), ready to be re-paired by anyone
// later, rather than disappearing along with its telemetry/schedule
// history. The regular user still loses it from their own device list the
// instant OwnerUserId clears (GET /devices' non-admin branch filters on
// that), which is the actual user-facing effect "delete" needs to have --
// there's just no server-side row deletion behind it anymore.
devicesGroup.MapDelete("/{id:guid}", async (Guid id, SynoraDbContext db, HttpContext http, IDeviceMqttPublisher mqtt) =>
{
    var isAdmin = http.User.IsInRole(nameof(UserRole.Admin)) || http.User.IsInRole(nameof(UserRole.SuperAdmin));
    var userId = CurrentUserId(http);
    var device = await db.Devices.FirstOrDefaultAsync(d => d.Id == id && (isAdmin || d.OwnerUserId == userId));
    if (device is null)
    {
        return Results.NotFound();
    }

    // Clears the previous owner's personalization -- a device sitting
    // unpaired/re-pairable in the system shouldn't keep showing whoever had
    // it last's chosen photo.
    if (device.AvatarPath is not null)
    {
        var avatarPath = Path.Combine(avatarStoragePath, device.AvatarPath);
        if (File.Exists(avatarPath))
        {
            File.Delete(avatarPath);
        }
        device.AvatarPath = null;
    }

    // Same reasoning as the avatar above -- the watering schedule and
    // low-water alert configuration are the previous owner's settings, not
    // something a new owner pairing this same physical device later should
    // inherit. Deleted/reset here (not just left for the next owner to
    // notice and change) since PublishStateAsync below re-reads schedules
    // fresh from the DB -- doing this first means the retained state the
    // device (and BLE deviceInfo) picks up already reflects "no schedules"
    // instead of a stale list belonging to someone else.
    var oldSchedules = await db.WateringSchedules.Where(s => s.DeviceId == id).ToListAsync();
    db.WateringSchedules.RemoveRange(oldSchedules);
    device.LowWaterWarningThresholdPercent = null;
    device.LowWaterCriticalThresholdPercent = null;
    device.LowWaterAlertIntervalMinutes = null;
    device.LowWaterLastAlertedAt = null;

    device.OwnerUserId = null;
    device.PairedAt = null;
    await db.SaveChangesAsync();
    // Tells the device it's unpaired -- PublishStateAsync re-reads the
    // device from the DB to build the payload, so this has to happen after
    // OwnerUserId actually clears. Without this, "deleting" a device here
    // left the firmware still showing the dashboard (isPaired cached true
    // client-side) with no way back to the pairing screen short of a
    // factory reset.
    await mqtt.PublishStateAsync(id);
    return Results.Ok();
}).RequireAuthorization();

devicesGroup.MapPost("/{id:guid}/avatar", async (Guid id, SynoraDbContext db, HttpContext http, IFormFile file) =>
{
    var userId = CurrentUserId(http);
    var device = await db.Devices.FirstOrDefaultAsync(d => d.Id == id && d.OwnerUserId == userId);
    if (device is null)
    {
        return Results.NotFound();
    }

    var extensionByContentType = new Dictionary<string, string>
    {
        ["image/jpeg"] = ".jpg",
        ["image/png"] = ".png",
        ["image/webp"] = ".webp",
    };
    if (!extensionByContentType.TryGetValue(file.ContentType, out var ext))
    {
        return Results.BadRequest("Unsupported image type. Use JPEG, PNG, or WebP.");
    }

    const long maxBytes = 8 * 1024 * 1024;
    if (file.Length is 0 or > maxBytes)
    {
        return Results.BadRequest("Image must be between 1 byte and 8MB.");
    }

    // Delete the previous avatar file if this upload has a different
    // extension (e.g. replacing a .png with a .jpg) -- otherwise the old
    // file would be orphaned on disk once AvatarPath is overwritten below.
    if (device.AvatarPath is not null)
    {
        var oldPath = Path.Combine(avatarStoragePath, device.AvatarPath);
        if (File.Exists(oldPath))
        {
            File.Delete(oldPath);
        }
    }

    var fileName = $"{id}{ext}";
    var fullPath = Path.Combine(avatarStoragePath, fileName);
    await using (var stream = File.Create(fullPath))
    {
        await file.CopyToAsync(stream);
    }

    device.AvatarPath = fileName;
    await db.SaveChangesAsync();

    return Results.Ok(new { avatarUrl = $"/devices/{id}/avatar" });
})
// Binding IFormFile auto-attaches antiforgery-validation metadata, which
// throws at request time unless antiforgery services/middleware are wired
// up (they aren't, anywhere in this app) or explicitly opted out here. Safe
// to opt out: this route is only ever called by the Flutter app's
// bearer-token multipart upload, never by the cookie-authenticated web
// console (which has no avatar upload feature), so there's no ambient-cookie
// request forgery surface to protect against.
.DisableAntiforgery()
.RequireAuthorization();

devicesGroup.MapGet("/{id:guid}/avatar", async (Guid id, SynoraDbContext db) =>
{
    var device = await db.Devices.FirstOrDefaultAsync(d => d.Id == id);
    if (device?.AvatarPath is null)
    {
        return Results.NotFound();
    }

    var fullPath = Path.Combine(avatarStoragePath, device.AvatarPath);
    if (!File.Exists(fullPath))
    {
        return Results.NotFound();
    }

    var contentType = Path.GetExtension(fullPath) switch
    {
        ".png" => "image/png",
        ".webp" => "image/webp",
        _ => "image/jpeg",
    };
    return Results.File(fullPath, contentType);
});

devicesGroup.MapPost("/{id:guid}/water", async (
    Guid id, SynoraDbContext db, HttpContext http, IDeviceMqttPublisher mqtt, TriggerWaterRequest request) =>
{
    var isAdmin = http.User.IsInRole(nameof(UserRole.Admin)) || http.User.IsInRole(nameof(UserRole.SuperAdmin));
    var userId = CurrentUserId(http);
    var device = await db.Devices.FirstOrDefaultAsync(d => d.Id == id && (isAdmin || d.OwnerUserId == userId));
    if (device is null)
    {
        return Results.NotFound();
    }
    if (request.DurationSeconds < 1 || request.DurationSeconds > 300)
    {
        return Results.BadRequest("DurationSeconds must be between 1 and 300.");
    }

    // One-shot pulse published straight to synora/devices/{id}/commands/water
    // (QoS 1, NOT retained) instead of setting a polled field -- there's no
    // ack channel back from the device, same fire-and-forget trust model as
    // before.
    await mqtt.PublishWaterCommandAsync(id, request.DurationSeconds);
    return Results.Ok();
}).RequireAuthorization();

devicesGroup.MapPost("/{id:guid}/ping", async (Guid id, SynoraDbContext db, HttpContext http, IDeviceMqttPublisher mqtt) =>
{
    var isAdmin = http.User.IsInRole(nameof(UserRole.Admin)) || http.User.IsInRole(nameof(UserRole.SuperAdmin));
    var userId = CurrentUserId(http);
    var device = await db.Devices.FirstOrDefaultAsync(d => d.Id == id && (isAdmin || d.OwnerUserId == userId));
    if (device is null)
    {
        return Results.NotFound();
    }

    await mqtt.PublishPingCommandAsync(id);
    return Results.Ok();
}).RequireAuthorization();

// One turn of the app's own "talk to Synora" assistant (device detail
// screen's talk button) -- STT the uploaded recording, run it through the
// same LlmVoicePipeline.SystemPrompt + real MCP tools as a device voice turn
// (via VoiceMqttDispatcher.GetOrCreateSession, same mechanism as the admin
// chat-test endpoint -- see that endpoint's comment for why registering the
// session matters), then TTS the reply back as a WAV the app can play
// immediately. Turn-based (one HTTP call per utterance) rather than
// streaming -- the app itself decides when a recording ends (silence
// timeout or explicit stop) and calls this once per utterance, looping on
// its own for the auto-relisten conversation.
devicesGroup.MapPost("/{id:guid}/assistant/turn", async (
    Guid id, HttpContext http, SynoraDbContext db, ISpeechToText speechToText, ILlmClient llmClient,
    ITextToSpeech textToSpeech, McpBridge mcpBridge, IVoicePublisher voicePublisher,
    VoiceMqttDispatcher voiceDispatcher, IFormFile file) =>
{
    var isAdmin = http.User.IsInRole(nameof(UserRole.Admin)) || http.User.IsInRole(nameof(UserRole.SuperAdmin));
    var userId = CurrentUserId(http);
    var device = await db.Devices.FirstOrDefaultAsync(d => d.Id == id && (isAdmin || d.OwnerUserId == userId));
    if (device is null)
    {
        return Results.NotFound();
    }

    const int sampleRateHz = 16000; // matches the firmware's SynoraVoiceSession / LlmVoicePipeline
    using var stream = new MemoryStream();
    await file.CopyToAsync(stream);

    short[] pcm;
    try
    {
        pcm = OpusAudioDecoder.ParseWav16Mono(stream.ToArray(), sampleRateHz);
    }
    catch (InvalidDataException ex)
    {
        return Results.BadRequest($"Invalid WAV file: {ex.Message} (expected mono 16-bit PCM at {sampleRateHz} Hz).");
    }

    var transcript = await speechToText.TranscribeAsync(pcm, sampleRateHz, http.RequestAborted);
    if (string.IsNullOrWhiteSpace(transcript))
    {
        // Empty transcript (silence/noise the ASR couldn't make anything of)
        // -- ask the user to repeat rather than sending nothing to the LLM,
        // same reasoning LlmVoicePipeline would hit on a blank turn.
        return Results.Ok(new AssistantTurnResponse("", "Mình chưa nghe rõ, bạn nói lại được không nhỉ?", null));
    }

    var session = voiceDispatcher.GetOrCreateSession(id, voicePublisher);
    var tools = await mcpBridge.GetToolsAsync(id, session, http.RequestAborted);
    var reply = await llmClient.GetReplyAsync(
        LlmVoicePipeline.SystemPrompt, transcript, tools,
        (name, args, ct) => mcpBridge.CallToolAsync(id, session, name, args, ct),
        http.RequestAborted);

    string? replyAudioWavBase64 = null;
    var (replyPcm, replySampleRateHz) = await textToSpeech.SynthesizeAsync(reply, http.RequestAborted);
    if (replyPcm.Length > 0)
    {
        replyAudioWavBase64 = Convert.ToBase64String(OpusAudioDecoder.BuildWav(replyPcm, replySampleRateHz));
    }

    // Persisted the same way the admin chat-test endpoint does, so a
    // conversation held from the app shows up in this device's normal chat
    // history (GET /devices/{id}/discussions) alongside real voice turns.
    var assistantSessionId = $"app-assistant-{Guid.NewGuid()}";
    string? userAudioFileName = null;
    if (pcm.Length > 0)
    {
        // Best-effort, same file naming/format as LlmVoicePipeline.TryWriteUserAudio.
        try
        {
            var audioFileName = $"{assistantSessionId}-{Guid.NewGuid():N}.wav";
            await File.WriteAllBytesAsync(
                Path.Combine(recordingsStoragePath, audioFileName), OpusAudioDecoder.BuildWav(pcm, sampleRateHz));
            userAudioFileName = audioFileName;
        }
        catch (Exception ex)
        {
            app.Logger.LogWarning(ex, "Failed to write assistant-turn user audio for device {DeviceId}", id);
        }
    }

    db.DiscussionMessages.Add(new DiscussionMessage
    {
        DeviceId = id,
        SessionId = assistantSessionId,
        UserText = transcript,
        AssistantText = reply,
        CreatedAt = DateTimeOffset.UtcNow,
        UserAudioPath = userAudioFileName,
    });
    await db.SaveChangesAsync();

    return Results.Ok(new AssistantTurnResponse(transcript, reply, replyAudioWavBase64));
})
// Same reasoning as /voice-test/stt's DisableAntiforgery(): no antiforgery
// middleware is registered anywhere in this app, so IFormFile's
// auto-attached antiforgery check would just throw looking for state that
// was never configured -- this doesn't turn off protection that existed.
.RequireAuthorization()
.DisableAntiforgery();

devicesGroup.MapPut("/{id:guid}/muted", async (
    Guid id, SynoraDbContext db, HttpContext http, IDeviceMqttPublisher mqtt, SetMutedRequest request) =>
{
    var isAdmin = http.User.IsInRole(nameof(UserRole.Admin)) || http.User.IsInRole(nameof(UserRole.SuperAdmin));
    var userId = CurrentUserId(http);
    var device = await db.Devices.FirstOrDefaultAsync(d => d.Id == id && (isAdmin || d.OwnerUserId == userId));
    if (device is null)
    {
        return Results.NotFound();
    }

    device.Muted = request.Muted;
    await db.SaveChangesAsync();
    await mqtt.PublishStateAsync(id);
    return Results.Ok();
}).RequireAuthorization();

// GET /{id}/pending-command (device-trust-model, polled) is gone -- muted/
// isPaired/schedules are now pushed to the DEVICE via the retained
// synora/devices/{id}/state MQTT topic (see MqttService), republished by
// this group's schedule CRUD endpoints below. GET /{id}/schedules below is
// a different consumer, though -- the app's device-detail screen still
// needs an on-demand list to render/edit, which the device-facing MQTT push
// doesn't serve.
devicesGroup.MapGet("/{id:guid}/schedules", async (Guid id, SynoraDbContext db, HttpContext http) =>
{
    var isAdmin = http.User.IsInRole(nameof(UserRole.Admin)) || http.User.IsInRole(nameof(UserRole.SuperAdmin));
    var userId = CurrentUserId(http);
    var device = await db.Devices.AsNoTracking().FirstOrDefaultAsync(d => d.Id == id && (isAdmin || d.OwnerUserId == userId));
    if (device is null)
    {
        return Results.NotFound();
    }

    var schedules = await db.WateringSchedules.AsNoTracking()
        .Where(s => s.DeviceId == id)
        .OrderBy(s => s.Hour).ThenBy(s => s.Minute)
        .Select(s => new { s.Id, s.Hour, s.Minute, s.DurationSeconds, s.Enabled })
        .ToListAsync();
    return Results.Ok(schedules);
}).RequireAuthorization();

devicesGroup.MapPost("/{id:guid}/schedules", async (
    Guid id, SynoraDbContext db, HttpContext http, IDeviceMqttPublisher mqtt, CreateWateringScheduleRequest request) =>
{
    var isAdmin = http.User.IsInRole(nameof(UserRole.Admin)) || http.User.IsInRole(nameof(UserRole.SuperAdmin));
    var userId = CurrentUserId(http);
    var device = await db.Devices.FirstOrDefaultAsync(d => d.Id == id && (isAdmin || d.OwnerUserId == userId));
    if (device is null)
    {
        return Results.NotFound();
    }
    if (request.Hour is < 0 or > 23 || request.Minute is < 0 or > 59 || request.DurationSeconds is <= 0 or > 300)
    {
        return Results.BadRequest("Invalid schedule: hour 0-23, minute 0-59, duration 1-300 seconds.");
    }

    var schedule = new WateringSchedule
    {
        DeviceId = id,
        Hour = request.Hour,
        Minute = request.Minute,
        DurationSeconds = request.DurationSeconds,
        Enabled = true,
    };
    db.WateringSchedules.Add(schedule);
    await db.SaveChangesAsync();
    await mqtt.PublishStateAsync(id);
    return Results.Ok(new { schedule.Id, schedule.Hour, schedule.Minute, schedule.DurationSeconds, schedule.Enabled });
}).RequireAuthorization();

devicesGroup.MapPatch("/{id:guid}/schedules/{scheduleId:guid}", async (
    Guid id, Guid scheduleId, SynoraDbContext db, HttpContext http, IDeviceMqttPublisher mqtt, UpdateWateringScheduleRequest request) =>
{
    var isAdmin = http.User.IsInRole(nameof(UserRole.Admin)) || http.User.IsInRole(nameof(UserRole.SuperAdmin));
    var userId = CurrentUserId(http);
    var device = await db.Devices.FirstOrDefaultAsync(d => d.Id == id && (isAdmin || d.OwnerUserId == userId));
    if (device is null)
    {
        return Results.NotFound();
    }
    var schedule = await db.WateringSchedules.FirstOrDefaultAsync(s => s.Id == scheduleId && s.DeviceId == id);
    if (schedule is null)
    {
        return Results.NotFound();
    }
    if (request.Hour is < 0 or > 23 || request.Minute is < 0 or > 59 || request.DurationSeconds is <= 0 or > 300)
    {
        return Results.BadRequest("Invalid schedule: hour 0-23, minute 0-59, duration 1-300 seconds.");
    }

    schedule.Hour = request.Hour;
    schedule.Minute = request.Minute;
    schedule.DurationSeconds = request.DurationSeconds;
    schedule.Enabled = request.Enabled;
    await db.SaveChangesAsync();
    await mqtt.PublishStateAsync(id);
    return Results.Ok();
}).RequireAuthorization();

devicesGroup.MapDelete("/{id:guid}/schedules/{scheduleId:guid}", async (
    Guid id, Guid scheduleId, SynoraDbContext db, HttpContext http, IDeviceMqttPublisher mqtt) =>
{
    var isAdmin = http.User.IsInRole(nameof(UserRole.Admin)) || http.User.IsInRole(nameof(UserRole.SuperAdmin));
    var userId = CurrentUserId(http);
    var device = await db.Devices.FirstOrDefaultAsync(d => d.Id == id && (isAdmin || d.OwnerUserId == userId));
    if (device is null)
    {
        return Results.NotFound();
    }
    var schedule = await db.WateringSchedules.FirstOrDefaultAsync(s => s.Id == scheduleId && s.DeviceId == id);
    if (schedule is null)
    {
        return Results.NotFound();
    }

    db.WateringSchedules.Remove(schedule);
    await db.SaveChangesAsync();
    await mqtt.PublishStateAsync(id);
    return Results.Ok();
}).RequireAuthorization();

// Admin test/debug tools for the web console's admin test page -- exercise
// the real pipeline pieces (ISpeechToText, ILlmClient, the MQTT client's own
// connection state) directly, without needing a real device on the other
// end. Same admin-or-superadmin gate as the device test-reading/water/ping
// endpoints above.
var adminGroup = app.MapGroup("/admin").RequireAuthorization();

adminGroup.MapGet("/mqtt-status", (HttpContext http, IDeviceMqttPublisher mqtt) =>
{
    if (!IsAdmin(http))
    {
        return Results.Forbid();
    }
    return Results.Ok(new MqttStatusResponse(mqtt.IsConnected));
});

// Runs an uploaded WAV file through the exact same ISpeechToText the real
// voice pipeline uses (LlmVoicePipeline.OnListenStopAsync) -- lets the ASR
// sidecar's wiring/model be checked from the web console without needing a
// device to actually capture and encode audio.
adminGroup.MapPost("/voice-test/stt", async (HttpContext http, ISpeechToText speechToText, IFormFile file) =>
{
    if (!IsAdmin(http))
    {
        return Results.Forbid();
    }

    const int sampleRateHz = 16000; // matches the firmware's SynoraVoiceSession / LlmVoicePipeline
    using var stream = new MemoryStream();
    await file.CopyToAsync(stream);

    short[] pcm;
    try
    {
        pcm = OpusAudioDecoder.ParseWav16Mono(stream.ToArray(), sampleRateHz);
    }
    catch (InvalidDataException ex)
    {
        return Results.BadRequest($"Invalid WAV file: {ex.Message} (expected mono 16-bit PCM at {sampleRateHz} Hz).");
    }

    var text = await speechToText.TranscribeAsync(pcm, sampleRateHz, http.RequestAborted);
    return Results.Ok(new SttTestResponse(text));
})
// Same reasoning as /devices/{id}/avatar's DisableAntiforgery(): no
// antiforgery middleware is registered anywhere in this app, so IFormFile's
// auto-attached antiforgery check would just throw looking for state that
// was never configured -- this doesn't turn off protection that existed.
.DisableAntiforgery();

// Runs typed text through the exact same ITextToSpeech the real voice
// pipeline uses (LlmVoicePipeline.OnListenStopAsync) and returns a WAV file
// directly, so the admin test page can play it back in a plain <audio>
// element.
adminGroup.MapPost("/voice-test/tts", async (HttpContext http, ITextToSpeech textToSpeech, TtsTestRequest request) =>
{
    if (!IsAdmin(http))
    {
        return Results.Forbid();
    }
    if (string.IsNullOrWhiteSpace(request.Text))
    {
        return Results.BadRequest("Text must not be empty.");
    }

    var (pcm, sampleRateHz) = await textToSpeech.SynthesizeAsync(request.Text, http.RequestAborted);
    if (pcm.Length == 0)
    {
        return Results.Problem(
            title: "TTS synthesis failed",
            detail: "The TTS sidecar returned nothing -- check server logs / that the tts service is up.",
            statusCode: StatusCodes.Status502BadGateway);
    }

    var wavBytes = OpusAudioDecoder.BuildWav(pcm, sampleRateHz);
    return Results.Bytes(wavBytes, "audio/wav");
});

// Runs a typed message through the exact same ILlmClient + system prompt as
// a real voice turn (LlmVoicePipeline.OnListenStopAsync), skipping STT/audio
// entirely -- for testing LLM prompt/response behavior in isolation. If
// deviceId is given, persists the exchange as a DiscussionMessage the same
// way a real turn does, so it shows up in that device's normal chat history
// (GET /devices/{id}/discussions) rather than needing separate test-only
// storage.
//
// MCP tools ARE wired up when a deviceId is given, via
// VoiceMqttDispatcher.GetOrCreateSession -- reuses a live device session if
// one's already active (a real voice turn in progress), otherwise registers
// a fresh ad-hoc one. Registration matters, not just construction: an
// unregistered VoiceSession's outgoing MCP request still reaches the device
// fine, but VoiceMqttDispatcher.HandleControlAsync drops the device's mcp
// reply for having no matching _sessions entry, and the call just times out
// (confirmed live). Without a deviceId there's nothing to route tool calls
// to, so it still runs with an empty tool list in that case (a tool-call
// attempt would just get a "not available" error back, same as a device that
// never answers).
adminGroup.MapPost("/voice-test/chat", async (
    HttpContext http, SynoraDbContext db, ILlmClient llmClient, McpBridge mcpBridge,
    IVoicePublisher voicePublisher, VoiceMqttDispatcher voiceDispatcher, ChatTestRequest request) =>
{
    if (!IsAdmin(http))
    {
        return Results.Forbid();
    }
    if (string.IsNullOrWhiteSpace(request.Message))
    {
        return Results.BadRequest("Message must not be empty.");
    }

    var tools = Array.Empty<Synora.Protocol.McpToolDefinition>() as IReadOnlyList<Synora.Protocol.McpToolDefinition>;
    Func<string, System.Text.Json.JsonElement, CancellationToken, Task<System.Text.Json.JsonElement>> callTool =
        (_, _, _) => Task.FromResult(System.Text.Json.JsonSerializer.SerializeToElement(
            new { error = new { message = "tools not available in chat-test mode" } }));

    if (request.DeviceId is Guid toolDeviceId)
    {
        var session = voiceDispatcher.GetOrCreateSession(toolDeviceId, voicePublisher);
        tools = await mcpBridge.GetToolsAsync(toolDeviceId, session, http.RequestAborted);
        callTool = (name, args, ct) => mcpBridge.CallToolAsync(toolDeviceId, session, name, args, ct);
    }

    var reply = await llmClient.GetReplyAsync(
        LlmVoicePipeline.SystemPrompt, request.Message, tools, callTool, http.RequestAborted);

    if (request.DeviceId is Guid deviceId)
    {
        var deviceExists = await db.Devices.AnyAsync(d => d.Id == deviceId);
        if (deviceExists)
        {
            db.DiscussionMessages.Add(new DiscussionMessage
            {
                DeviceId = deviceId,
                SessionId = $"admin-test-{Guid.NewGuid()}",
                UserText = request.Message,
                AssistantText = reply,
                CreatedAt = DateTimeOffset.UtcNow,
            });
            await db.SaveChangesAsync();
        }
    }

    return Results.Ok(new ChatTestResponse(reply));
});

// Retroactive cleanup for the case-mismatched-MAC duplicate rows
// NormalizeMac() (see /devices/activation above) stops from being created
// going forward but can't fix for rows that already exist from before that
// fix -- groups every device by the same normalized (uppercase) MAC and
// returns only the groups with more than one row, so an admin can see
// exactly which rows are the same physical device without querying the DB
// by hand. "Has related data" is surfaced per-row (not just a boolean on
// the group) so the admin can tell *which* row in a group is the safe one
// to remove via the hard-delete endpoint below.
adminGroup.MapGet("/duplicate-devices", async (SynoraDbContext db, HttpContext http) =>
{
    if (!IsAdmin(http))
    {
        return Results.Forbid();
    }

    var devices = await db.Devices
        .Include(d => d.Owner)
        .Where(d => d.MacAddress != null)
        .ToListAsync();

    var groups = devices
        .GroupBy(d => NormalizeMac(d.MacAddress)!)
        .Where(g => g.Count() > 1)
        .OrderBy(g => g.Key)
        .ToList();

    if (groups.Count == 0)
    {
        return Results.Ok(Array.Empty<DuplicateDeviceGroupDto>());
    }

    var deviceIds = groups.SelectMany(g => g.Select(d => d.Id)).ToList();
    var deviceIdsWithReadings = (await db.SensorReadings
        .Where(r => deviceIds.Contains(r.DeviceId))
        .Select(r => r.DeviceId)
        .Distinct()
        .ToListAsync())
        .ToHashSet();
    var deviceIdsWithSchedules = (await db.WateringSchedules
        .Where(s => deviceIds.Contains(s.DeviceId))
        .Select(s => s.DeviceId)
        .Distinct()
        .ToListAsync())
        .ToHashSet();

    var result = groups.Select(g => new DuplicateDeviceGroupDto(
        g.Key,
        g.OrderBy(d => d.CreatedAt)
         .Select(d => new DuplicateDeviceDto(
             d.Id,
             d.MacAddress,
             d.Name,
             d.OwnerUserId,
             d.Owner != null ? (d.Owner.Email ?? d.Owner.Username) : null,
             d.CreatedAt,
             d.PairedAt,
             deviceIdsWithReadings.Contains(d.Id) || deviceIdsWithSchedules.Contains(d.Id)))
         .ToList()))
        .ToList();

    return Results.Ok(result);
});

// Admin-only hard delete for cleaning up a specific bad/duplicate device
// row found via GET /admin/duplicate-devices above -- distinct from
// DELETE /devices/{id}, which deliberately only unpairs (see that
// endpoint's own comment) and is unsuitable for actually removing a
// leftover duplicate row from the table. Refuses when the device has any
// SensorReadings/WateringSchedules attached rather than silently cascading
// through them (both cascade-delete at the DB level per SynoraDbContext) --
// an admin should have to consciously accept losing real history, not have
// it vanish as a side effect of tidying up what looked like a dead row.
adminGroup.MapDelete("/devices/{id:guid}", async (Guid id, SynoraDbContext db, HttpContext http) =>
{
    if (!IsAdmin(http))
    {
        return Results.Forbid();
    }

    var device = await db.Devices.FirstOrDefaultAsync(d => d.Id == id);
    if (device is null)
    {
        return Results.NotFound();
    }

    var hasSensorReadings = await db.SensorReadings.AnyAsync(r => r.DeviceId == id);
    var hasSchedules = await db.WateringSchedules.AnyAsync(s => s.DeviceId == id);
    if (hasSensorReadings || hasSchedules)
    {
        var parts = new List<string>();
        if (hasSensorReadings) parts.Add("sensor readings");
        if (hasSchedules) parts.Add("watering schedules");
        return Results.Problem(
            title: "Refusing to hard-delete device with related data",
            detail: $"Device {id} still has {string.Join(" and ", parts)} attached -- this is real history, " +
                "not a dead leftover row. If it needs to stop being owned/visible to its current user, use " +
                "DELETE /devices/{id} (unpair) instead; only hard-delete rows confirmed to have zero related data.",
            statusCode: StatusCodes.Status409Conflict);
    }

    // Same avatar-file cleanup as DELETE /devices/{id} (unpair) -- otherwise
    // the file is orphaned on disk once the row disappears.
    if (device.AvatarPath is not null)
    {
        var avatarPath = Path.Combine(avatarStoragePath, device.AvatarPath);
        if (File.Exists(avatarPath))
        {
            File.Delete(avatarPath);
        }
    }

    // DiscussionMessages/MicTestUploads cascade-delete with the row, so collect
    // their WAV paths first and remove the files once the delete has committed.
    var recordingFiles = await db.DiscussionMessages
        .Where(m => m.DeviceId == id && m.UserAudioPath != null)
        .Select(m => m.UserAudioPath!)
        .ToListAsync();
    var micTestFiles = await db.MicTestUploads
        .Where(m => m.DeviceId == id)
        .Select(m => m.FilePath)
        .ToListAsync();

    db.Devices.Remove(device);
    await db.SaveChangesAsync();

    foreach (var fullPath in recordingFiles.Select(f => Path.Combine(recordingsStoragePath, f))
                 .Concat(micTestFiles.Select(f => Path.Combine(micTestStoragePath, f))))
    {
        try
        {
            File.Delete(fullPath);
        }
        catch (Exception ex)
        {
            app.Logger.LogWarning(ex, "Failed to delete {Path} after hard-deleting device {DeviceId}", fullPath, id);
        }
    }
    return Results.Ok();
});

// Global, admin-editable config not tied to any one device/user -- see
// SystemSettings' doc comment. SuperAdminOnly (stricter than the
// admin-or-superadmin gate on the test tools above) since this affects
// every device/user at once, matching the /users group's own policy for
// the same reason.
adminGroup.MapGet("/system-settings", async (SynoraDbContext db, CancellationToken ct) =>
{
    var settings = await db.SystemSettings.FindAsync([1], ct);
    return Results.Ok(new SystemSettingsDto(settings?.TtsVoice ?? "Trúc Ly"));
}).RequireAuthorization("SuperAdminOnly");

adminGroup.MapPut("/system-settings", async (
    SynoraDbContext db, IHttpClientFactory httpClientFactory, IOptions<TtsOptions> ttsOptions,
    UpdateSystemSettingsRequest request, CancellationToken ct) =>
{
    if (string.IsNullOrWhiteSpace(request.TtsVoice))
    {
        return Results.BadRequest("TtsVoice must not be empty.");
    }

    // Cross-check against the sidecar's own live voice list rather than
    // trusting the string outright -- catches a typo/stale name immediately
    // instead of at the next voice reply.
    var voices = await FetchTtsVoicesAsync(httpClientFactory, ttsOptions.Value, ct);
    if (voices is not null && !voices.Any(v => v.Id == request.TtsVoice))
    {
        return Results.BadRequest($"Unknown voice '{request.TtsVoice}'. Available: {string.Join(", ", voices.Select(v => v.Id))}");
    }

    var settings = await db.SystemSettings.FindAsync([1], ct);
    if (settings is null)
    {
        return Results.NotFound();
    }

    settings.TtsVoice = request.TtsVoice;
    await db.SaveChangesAsync(ct);
    return Results.Ok(new SystemSettingsDto(settings.TtsVoice));
}).RequireAuthorization("SuperAdminOnly");

// Passthrough to the TTS sidecar's own GET /voices -- the admin system
// config page's voice picker is populated from this rather than a
// hand-maintained duplicate list, so it can't drift from what the sidecar
// actually has loaded.
adminGroup.MapGet("/tts-voices", async (IHttpClientFactory httpClientFactory, IOptions<TtsOptions> ttsOptions, CancellationToken ct) =>
{
    var voices = await FetchTtsVoicesAsync(httpClientFactory, ttsOptions.Value, ct);
    if (voices is null)
    {
        return Results.Problem(
            title: "Could not reach TTS sidecar",
            detail: "Check server logs / that the tts service is up.",
            statusCode: StatusCodes.Status502BadGateway);
    }
    return Results.Ok(voices);
}).RequireAuthorization("SuperAdminOnly");

// Admin-only, not owner-or-admin like the regular per-device endpoints
// (GET /devices/{id}/discussions etc.) -- this is a fleet-wide hardware
// diagnostic tool (firmware's `mictest` console command, see MqttService)
// for whoever administers the fleet, not something a regular device owner
// needs. Lists across ALL devices, newest-first, same 100-row cap as
// GET /devices/{id}/discussions.
adminGroup.MapGet("/mictest-uploads", async (SynoraDbContext db, HttpContext http) =>
{
    if (!IsAdmin(http))
    {
        return Results.Forbid();
    }

    var uploads = await db.MicTestUploads
        .Include(m => m.Device)
        .OrderByDescending(m => m.CreatedAt)
        .Take(100)
        .Select(m => new MicTestUploadDto(
            m.Id, m.DeviceId, m.Device != null ? m.Device.Name : "(deleted device)", m.CreatedAt, m.SizeBytes))
        .ToListAsync();

    return Results.Ok(uploads);
});

// Serves the raw WAV file for one mictest upload -- same admin-only gating
// as GET /admin/mictest-uploads above (this is a bare upload-id lookup,
// unlike GET /devices/{id}/discussions/{messageId}/audio, since there's no
// owner to scope it under).
adminGroup.MapGet("/mictest-uploads/{id:guid}/audio", async (Guid id, SynoraDbContext db, HttpContext http) =>
{
    if (!IsAdmin(http))
    {
        return Results.Forbid();
    }

    var upload = await db.MicTestUploads.FirstOrDefaultAsync(m => m.Id == id);
    if (upload is null)
    {
        return Results.NotFound();
    }

    var fullPath = Path.Combine(micTestStoragePath, upload.FilePath);
    if (!File.Exists(fullPath))
    {
        return Results.NotFound();
    }

    return Results.File(fullPath, "audio/wav");
});

// Admin-only firmware upload -- validates .bin extension, computes the
// sha256 the device will be told to verify against (see
// IDeviceMqttPublisher.PublishOtaCommandAsync / BLE ota_start), and stores
// the file under Firmware:StoragePath keyed by its own Id rather than its
// (free-form, not filename-safe) version string. Versions are unique: a
// re-upload of an existing version is rejected with 409 before anything is
// written to disk. A .zip is a flash kit (scripts/package-firmware.sh): its
// app image becomes the OTA .bin and the rest is kept for USB flashing.
adminGroup.MapPost("/firmware", async (HttpContext http, SynoraDbContext db, IFormFile file, [FromForm] string version, [FromForm] string? notes) =>
{
    if (!IsAdmin(http))
    {
        return Results.Forbid();
    }
    var isKit = file.FileName.EndsWith(".zip", StringComparison.OrdinalIgnoreCase);
    if (!isKit && !file.FileName.EndsWith(".bin", StringComparison.OrdinalIgnoreCase))
    {
        return Results.BadRequest("Firmware file must be a .bin app image or a flash-kit .zip.");
    }
    if (file.Length == 0)
    {
        return Results.BadRequest("Firmware file is empty.");
    }
    if (string.IsNullOrWhiteSpace(version))
    {
        return Results.BadRequest("Version is required.");
    }
    if (await db.FirmwareBuilds.AnyAsync(f => f.Version == version))
    {
        return Results.Conflict($"Firmware version '{version}' already exists.");
    }
    if (isKit)
    {
        return await SaveFlashKitBuildAsync(db, file, version, notes);
    }
    if (file.Length > 8 * 1024 * 1024)
    {
        return Results.StatusCode(StatusCodes.Status413PayloadTooLarge);
    }

    const byte EspImageMagic = 0xE9;
    await using (var header = file.OpenReadStream())
    {
        if (header.ReadByte() != EspImageMagic)
        {
            return Results.BadRequest("Not an ESP32 app image (missing 0xE9 magic byte).");
        }
    }

    var id = Guid.NewGuid();
    var fileName = $"{id}.bin";
    var fullPath = Path.Combine(firmwareStoragePath, fileName);

    string sha256Hex;
    using (var sha256 = SHA256.Create())
    await using (var stream = File.Create(fullPath))
    {
        await file.CopyToAsync(stream);
        stream.Position = 0;
        var hash = await sha256.ComputeHashAsync(stream);
        sha256Hex = Convert.ToHexStringLower(hash);
    }

    var build = new FirmwareBuild
    {
        Id = id,
        Version = version,
        FileName = fileName,
        Sha256 = sha256Hex,
        SizeBytes = file.Length,
        CreatedAt = DateTimeOffset.UtcNow,
        Notes = notes,
    };
    db.FirmwareBuilds.Add(build);
    try
    {
        await db.SaveChangesAsync();
    }
    catch
    {
        File.Delete(fullPath);
        throw;
    }

    return Results.Ok(new FirmwareBuildDto(build.Id, build.Version, build.SizeBytes, build.Sha256, build.CreatedAt, build.Notes, build.HasFlashKit));
})
// Same reasoning as /devices/{id}/avatar's DisableAntiforgery(): no
// antiforgery middleware is registered anywhere in this app, so IFormFile's
// auto-attached antiforgery metadata would otherwise throw at request time.
.DisableAntiforgery()
// Plain .bin uploads are capped at 8 MB in the handler; flash kits carry
// bootloader + srmodels too, so the request limit is the kit's.
.WithMetadata(new RequestSizeLimitAttribute(FlashKit.MaxTotalBytes));

async Task<IResult> SaveFlashKitBuildAsync(SynoraDbContext db, IFormFile file, string version, string? notes)
{
    var id = Guid.NewGuid();
    var appFileName = $"{id}.bin";
    var appPath = Path.Combine(firmwareStoragePath, appFileName);
    var kitDir = FlashKitEndpoints.KitDir(firmwareStoragePath, id);

    void Cleanup()
    {
        if (File.Exists(appPath)) File.Delete(appPath);
        if (Directory.Exists(kitDir)) Directory.Delete(kitDir, recursive: true);
    }

    string appSha;
    long appSize;
    try
    {
        await using var upload = file.OpenReadStream();
        (_, appSha, appSize) = await FlashKit.ExtractAsync(upload, kitDir, appPath);
    }
    catch (FlashKitException ex)
    {
        Cleanup();
        return Results.BadRequest(ex.Message);
    }

    var build = new FirmwareBuild
    {
        Id = id,
        Version = version,
        FileName = appFileName,
        Sha256 = appSha,
        SizeBytes = appSize,
        CreatedAt = DateTimeOffset.UtcNow,
        Notes = notes,
        HasFlashKit = true,
    };
    db.FirmwareBuilds.Add(build);
    try
    {
        await db.SaveChangesAsync();
    }
    catch
    {
        Cleanup();
        throw;
    }
    return Results.Ok(new FirmwareBuildDto(build.Id, build.Version, build.SizeBytes, build.Sha256, build.CreatedAt, build.Notes, build.HasFlashKit));
}

adminGroup.MapFlashKit(firmwareStoragePath);

adminGroup.MapGet("/firmware", async (HttpContext http, SynoraDbContext db) =>
{
    if (!IsAdmin(http))
    {
        return Results.Forbid();
    }

    var builds = await db.FirmwareBuilds
        .OrderByDescending(f => f.CreatedAt)
        .Select(f => new FirmwareBuildDto(f.Id, f.Version, f.SizeBytes, f.Sha256, f.CreatedAt, f.Notes, f.HasFlashKit))
        .ToListAsync();
    return Results.Ok(builds);
});

adminGroup.MapDelete("/firmware/{id:guid}", async (Guid id, HttpContext http, SynoraDbContext db, ILogger<Program> logger) =>
{
    if (!IsAdmin(http))
    {
        return Results.Forbid();
    }

    var build = await db.FirmwareBuilds.FirstOrDefaultAsync(f => f.Id == id);
    if (build is null)
    {
        return Results.NotFound();
    }

    db.FirmwareBuilds.Remove(build);
    await db.SaveChangesAsync();

    // Row first, file second: a leftover .bin is harmless, a row pointing at a
    // missing file would 404 a device mid-OTA.
    var fullPath = Path.Combine(firmwareStoragePath, build.FileName);
    var kitDir = FlashKitEndpoints.KitDir(firmwareStoragePath, build.Id);
    try
    {
        File.Delete(fullPath);
        if (Directory.Exists(kitDir)) Directory.Delete(kitDir, recursive: true);
    }
    catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
    {
        logger.LogWarning(ex, "Could not delete firmware files for build {Id}", build.Id);
    }

    return Results.NoContent();
});

// Unauthenticated -- the device has no user session to present here, and the
// binary itself has no secrets baked in (no WiFi creds or keys, just
// esp_crt_bundle's public CA list). Integrity is covered by the sha256 sent
// alongside the download URL (commands/ota payload or BLE ota_start), which
// esp_https_ota/the BLE commit path verify before flashing.
app.MapGet("/firmware/{id:guid}/binary", async (Guid id, SynoraDbContext db) =>
{
    var build = await db.FirmwareBuilds.FirstOrDefaultAsync(f => f.Id == id);
    if (build is null)
    {
        return Results.NotFound();
    }

    var fullPath = Path.Combine(firmwareStoragePath, build.FileName);
    if (!File.Exists(fullPath))
    {
        return Results.NotFound();
    }

    return Results.File(fullPath, "application/octet-stream");
});

// Triggers the WiFi-OTA path (commands/ota, see MqttService) -- the BLE-push
// fallback for a no-WiFi/unprovisioned device doesn't go through the server
// at all, it's a direct app<->device GATT transfer.
devicesGroup.MapPost("/{id:guid}/ota", async (
    Guid id, SynoraDbContext db, HttpContext http, IDeviceMqttPublisher mqtt, TriggerOtaRequest request) =>
{
    var isAdmin = http.User.IsInRole(nameof(UserRole.Admin)) || http.User.IsInRole(nameof(UserRole.SuperAdmin));
    var userId = CurrentUserId(http);
    var device = await db.Devices.FirstOrDefaultAsync(d => d.Id == id && (isAdmin || d.OwnerUserId == userId));
    if (device is null)
    {
        return Results.NotFound();
    }

    var build = await db.FirmwareBuilds.FirstOrDefaultAsync(f => f.Id == request.FirmwareBuildId);
    if (build is null)
    {
        return Results.BadRequest("Unknown firmware build.");
    }

    // PublishAsync silently drops when disconnected, so surface that instead of a false 200.
    if (!mqtt.IsConnected)
    {
        return Results.Problem(
            title: "MQTT broker unavailable",
            detail: "The OTA command could not be delivered -- try again shortly.",
            statusCode: StatusCodes.Status503ServiceUnavailable);
    }

    var baseUrl = string.IsNullOrEmpty(firmwarePublicBaseUrl)
        ? $"{http.Request.Scheme}://{http.Request.Host}"
        : firmwarePublicBaseUrl;
    var downloadUrl = $"{baseUrl}/firmware/{build.Id}/binary";
    await mqtt.PublishOtaCommandAsync(id, build.Version, downloadUrl);
    return Results.Ok();
}).RequireAuthorization();

// Landing-site CMS (/cms/*) -- endpoints + one-time legacy SQLite import live in Cms/.
app.MapCms();

app.Run();

static async Task<List<TtsVoiceDto>?> FetchTtsVoicesAsync(IHttpClientFactory httpClientFactory, TtsOptions ttsOptions, CancellationToken ct)
{
    try
    {
        var client = httpClientFactory.CreateClient();
        return await client.GetFromJsonAsync<List<TtsVoiceDto>>($"{ttsOptions.BaseUrl}/voices", ct);
    }
    catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException)
    {
        return null;
    }
}

// Moved to Auth/SessionAuth.cs so the landing CMS login (Cms/CmsEndpoints.cs)
// shares the exact same password check and cookie principal shape.
static bool VerifyPassword(User? user, string password) => SessionAuth.VerifyPassword(user, password);

static ClaimsPrincipal BuildPrincipal(User user, string scheme) => SessionAuth.BuildPrincipal(user, scheme);

static string IssueJwt(User user, string signingKey)
{
    var claims = new[]
    {
        new Claim(JwtRegisteredClaimNames.Sub, user.Id.ToString()),
        new Claim(ClaimTypes.Name, user.Username),
        new Claim(ClaimTypes.Role, user.Role.ToString()),
    };

    var credentials = new SigningCredentials(
        new SymmetricSecurityKey(System.Text.Encoding.UTF8.GetBytes(signingKey)),
        SecurityAlgorithms.HmacSha256);

    var token = new JwtSecurityToken(
        issuer: JwtIssuer,
        audience: JwtAudience,
        claims: claims,
        expires: DateTime.UtcNow.AddDays(30),
        signingCredentials: credentials);

    return new JwtSecurityTokenHandler().WriteToken(token);
}

static Guid CurrentUserId(HttpContext http) =>
    Guid.Parse(http.User.FindFirstValue(ClaimTypes.NameIdentifier)!);

// Not used by the pre-existing device endpoints above (they repeat the
// inline role check directly) -- added for the newer /admin group so those
// don't have to repeat it a fourth+ time; left the older call sites alone
// rather than retrofitting them for an unrelated change.
// Accepts "example.com", "https://example.com/" etc.; returns the bare host or null.
static string? NormalizeDomain(string? value)
{
    if (string.IsNullOrWhiteSpace(value)) return null;
    var trimmed = value.Trim();
    var host = Uri.TryCreate(trimmed.Contains("://") ? trimmed : $"https://{trimmed}", UriKind.Absolute, out var uri)
        ? uri.Authority
        : trimmed.TrimEnd('/');
    return host.Length > 0 ? host : null;
}

static bool IsAdmin(HttpContext http) =>
    http.User.IsInRole(nameof(UserRole.Admin)) || http.User.IsInRole(nameof(UserRole.SuperAdmin));

static IResult NotConfigured(string feature) =>
    Results.Problem(
        title: $"{feature} not configured",
        detail: "Ask the admin to check the server logs.",
        statusCode: StatusCodes.Status503ServiceUnavailable);

record LoginRequest(string Username, string Password, bool? RememberMe = null);
record RegisterRequest(string Username, string? Email, string Password, bool? RememberMe = null);
record GoogleLoginRequest(string IdToken);
record FacebookLoginRequest(string AccessToken);
record CreateUserRequest(string Username, string Password, UserRole Role);
record PairDeviceRequest(string ActivationCode, string? MacAddress);
record ActivateDeviceRequest(string? MacAddress);
record RenameDeviceRequest(string Name);
record UpdateAlertsRequest(int? LowWaterWarningThresholdPercent, int? LowWaterCriticalThresholdPercent, int? LowWaterAlertIntervalMinutes);
record UpdateProfileRequest(string? Email);
record ChangePasswordRequest(string CurrentPassword, string NewPassword);
record ResetPasswordRequest(string NewPassword);
record TriggerWaterRequest(int DurationSeconds);
record TriggerOtaRequest(Guid FirmwareBuildId);
record SetMutedRequest(bool Muted);
// ReplyAudioWavBase64 is null when TTS synthesis failed -- the app should
// still show/read the transcript+reply text, just skip audio playback.
record AssistantTurnResponse(string Transcript, string Reply, string? ReplyAudioWavBase64);
record TelemetryRequest(
    double? TemperatureCelsius,
    double? HumidityPercent,
    double? SoilMoisturePercent,
    double? WaterLevelPercent,
    string? XiaozhiActivationCode = null);
record FcmTokenRequest(string? Token, string? Platform = null);
record SendTestNotificationRequest(string Title, string Body);
record CreateWateringScheduleRequest(int Hour, int Minute, int DurationSeconds);
record UpdateWateringScheduleRequest(int Hour, int Minute, int DurationSeconds, bool Enabled);
record MqttStatusResponse(bool Connected);
record SttTestResponse(string Text);
record TtsTestRequest(string Text);
record ChatTestRequest(string Message, Guid? DeviceId);
record ChatTestResponse(string Reply);
record SystemSettingsDto(string TtsVoice);
record UpdateSystemSettingsRequest(string TtsVoice);
record TtsVoiceDto(string Id, string Description, string Gender);

record UserSummaryDto(Guid Id, string Username, string? Email, string Role, DateTimeOffset CreatedAt, bool HasPushToken);

record SensorReadingDto(
    double? TemperatureCelsius,
    double? HumidityPercent,
    double? SoilMoisturePercent,
    double? WaterLevelPercent,
    DateTimeOffset RecordedAt);

record DiscussionMessageDto(
    Guid Id,
    string SessionId,
    string UserText,
    string AssistantText,
    DateTimeOffset CreatedAt,
    bool HasUserAudio);

record MicTestUploadDto(
    Guid Id,
    Guid DeviceId,
    string DeviceName,
    DateTimeOffset CreatedAt,
    long SizeBytes);

record FirmwareBuildDto(
    Guid Id,
    string Version,
    long SizeBytes,
    string Sha256,
    DateTimeOffset CreatedAt,
    string? Notes,
    bool HasFlashKit);

// Named records, not anonymous types, for GET /devices and GET /devices/{id}
// -- Swashbuckle can't generate a valid $ref name for an anonymous type that
// wraps another anonymous type (LatestReading), which broke Swagger UI
// rendering entirely ("Unable to render this definition").
record DeviceSummaryDto(
    Guid Id,
    string Name,
    string? MacAddress,
    DateTimeOffset? PairedAt,
    int? LowWaterWarningThresholdPercent,
    int? LowWaterCriticalThresholdPercent,
    int? LowWaterAlertIntervalMinutes,
    string? AvatarUrl,
    string? Owner,
    SensorReadingDto? LatestReading,
    string CareSuggestion,
    bool IsOnline,
    string? XiaozhiActivationCode,
    bool Muted,
    string? FirmwareVersion);

// GET /admin/duplicate-devices -- one row per device sharing a normalized
// MAC with at least one other device, grouped under that MAC. HasRelatedData
// is true if the device has any SensorReadings or WateringSchedules, the
// same condition DELETE /admin/devices/{id} (hard delete) refuses on.
record DuplicateDeviceGroupDto(string NormalizedMacAddress, List<DuplicateDeviceDto> Devices);

record DuplicateDeviceDto(
    Guid Id,
    string? MacAddress,
    string Name,
    Guid? OwnerUserId,
    string? OwnerEmail,
    DateTimeOffset CreatedAt,
    DateTimeOffset? PairedAt,
    bool HasRelatedData);

sealed class FacebookProfile
{
    [JsonPropertyName("id")]
    public string Id { get; set; } = "";

    [JsonPropertyName("email")]
    public string? Email { get; set; }
}

sealed class FacebookDebugTokenResponse
{
    [JsonPropertyName("data")]
    public FacebookDebugTokenData? Data { get; set; }
}

sealed class FacebookDebugTokenData
{
    [JsonPropertyName("app_id")]
    public string? AppId { get; set; }

    [JsonPropertyName("is_valid")]
    public bool IsValid { get; set; }
}

public partial class Program;
