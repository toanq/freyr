using FirebaseAdmin;
using FirebaseAdmin.Messaging;
using Microsoft.EntityFrameworkCore;
using Synora.Infrastructure;

namespace Synora.Api.Services;

public enum PushSendOutcome
{
    Sent,
    // FCM says the token is dead (app uninstalled, browser unsubscribed) -- delete it.
    InvalidToken,
    Failed,
}

public sealed record PushMessage(string Title, string Body, IReadOnlyDictionary<string, string>? Data = null, string? Link = null);

// Seam over FirebaseMessaging so fan-out/cleanup can be tested without Firebase.
public interface IPushSender
{
    bool IsConfigured { get; }
    Task<PushSendOutcome> SendAsync(string token, PushMessage message, CancellationToken ct = default);
}

public sealed class FirebasePushSender(ILogger<FirebasePushSender> logger) : IPushSender
{
    public bool IsConfigured => FirebaseApp.DefaultInstance is not null;

    public async Task<PushSendOutcome> SendAsync(string token, PushMessage message, CancellationToken ct = default)
    {
        var fcm = new Message
        {
            Token = token,
            Notification = new Notification { Title = message.Title, Body = message.Body },
            Data = message.Data,
            // Browsers only: icon + click-through target. Ignored for app tokens.
            Webpush = new WebpushConfig
            {
                Notification = new WebpushNotification { Icon = "/icons/icon-192.png" },
                FcmOptions = message.Link is null ? null : new WebpushFcmOptions { Link = message.Link },
            },
        };
        try
        {
            await FirebaseMessaging.DefaultInstance.SendAsync(fcm, ct);
            return PushSendOutcome.Sent;
        }
        catch (FirebaseMessagingException ex) when (
            ex.MessagingErrorCode is MessagingErrorCode.Unregistered or MessagingErrorCode.InvalidArgument)
        {
            logger.LogInformation("Push token rejected by FCM ({Code}); removing it", ex.MessagingErrorCode);
            return PushSendOutcome.InvalidToken;
        }
        catch (FirebaseMessagingException ex)
        {
            logger.LogWarning(ex, "Push send failed");
            return PushSendOutcome.Failed;
        }
    }
}

public sealed record PushFanOutResult(int Sent, int Removed, int Failed)
{
    public int Attempted => Sent + Removed + Failed;
}

// Sends one message to every registered token of a user, dropping tokens FCM
// reports as dead. Caller saves the DbContext.
public sealed class PushNotifier(IPushSender sender, IConfiguration configuration)
{
    public bool IsConfigured => sender.IsConfigured;

    // Absolute https link for a browser notification click, if Domain:App is known.
    public string? AppLink(string pathAndQuery)
    {
        var domain = configuration["Domain:App"]?.Trim().TrimEnd('/');
        if (string.IsNullOrEmpty(domain)) return null;
        var host = domain.Contains("://") ? new Uri(domain).Authority : domain;
        return $"https://{host}{pathAndQuery}";
    }

    public async Task<PushFanOutResult> SendToUserAsync(
        SynoraDbContext db, Guid userId, PushMessage message, CancellationToken ct = default)
    {
        var tokens = await db.PushTokens.Where(t => t.UserId == userId).ToListAsync(ct);
        int sent = 0, removed = 0, failed = 0;
        foreach (var token in tokens)
        {
            switch (await sender.SendAsync(token.Token, message, ct))
            {
                case PushSendOutcome.Sent:
                    sent++;
                    break;
                case PushSendOutcome.InvalidToken:
                    db.PushTokens.Remove(token);
                    removed++;
                    break;
                default:
                    failed++;
                    break;
            }
        }
        return new PushFanOutResult(sent, removed, failed);
    }
}
