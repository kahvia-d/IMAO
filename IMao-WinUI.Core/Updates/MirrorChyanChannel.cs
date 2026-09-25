#nullable enable
using System.Text.Json;
using System.Text.Json.Serialization;

namespace IMao_WinUI.Core.Updates;

/// <summary>
/// MirrorChyan's business error codes, plus the three states this client can be in before a code ever
/// arrives. Values 0 and above are the protocol's own, taken from its ErrorCode.md; the negative ones
/// are ours and cannot collide, because the protocol reserves negative codes for "unexpected severe
/// error" and that case is reported as <see cref="Unexpected"/> instead.
/// </summary>
public enum MirrorChyanError
{
    None = 0,
    Undivided = 1,
    InvalidParams = 1001,
    KeyExpired = 7001,
    KeyInvalid = 7002,
    QuotaExhausted = 7003,
    KeyMismatched = 7004,
    KeyBlocked = 7005,
    ResourceNotFound = 8001,
    InvalidOs = 8002,
    InvalidArch = 8003,
    InvalidChannel = 8004,
    /// <summary>The server answered with a negative code, which its documentation calls severe.</summary>
    Unexpected = -1,
    /// <summary>The request never produced a usable answer.</summary>
    Unreachable = -2,
    /// <summary>Something answered, but not with a shape this client understands.</summary>
    Malformed = -3,
}

/// <summary>
/// One MirrorChyan check reduced to what the client can act on. Pure data: no UI wording beyond a
/// user-facing <see cref="Message"/>, and no decision about whether to use the download.
/// </summary>
public sealed record MirrorChyanResult
{
    public MirrorChyanError Error { get; init; }
    public string Message { get; init; } = "";
    /// <summary>Latest published version, normalized to the four-part form the program uses. Empty when unknown.</summary>
    public string Version { get; init; } = "";
    /// <summary>Player-facing release notes. Empty when the server had none, including its literal "placeholder".</summary>
    public string ReleaseNote { get; init; } = "";
    public string? DownloadUrl { get; init; }
    public string? Sha256 { get; init; }
    public long? Size { get; init; }
    /// <summary><c>full</c>, <c>incremental</c>, or empty when the answer carried no package at all.</summary>
    public string UpdateType { get; init; } = "";
    public DateTimeOffset? CdkExpiresAt { get; init; }
    public bool HasUpdate { get; init; }

    /// <summary>
    /// True when MirrorChyan said it would serve a whole package. MirrorChyan builds the incremental
    /// package lazily, on the first request for a version pair, and answers with the full package while
    /// it does; asking once more a little later is what turns this into an incremental one.
    /// </summary>
    public bool ShouldAskAgainForIncremental => UpdateType == MirrorChyanChannel.FullPackage;

    /// <summary>
    /// True for the CDK errors the integration guide says must never stop a player from updating through
    /// another source.
    /// </summary>
    public bool IsCdkProblem => Error is MirrorChyanError.KeyExpired or MirrorChyanError.KeyInvalid
        or MirrorChyanError.QuotaExhausted or MirrorChyanError.KeyMismatched or MirrorChyanError.KeyBlocked;
}

/// <summary>
/// A MirrorChyan package that has been checked against the release it claims to be. It exists only after
/// the served version was found to equal the version the signed catalog described, so the caller may treat
/// it as "somewhere to fetch this release's bytes", and nothing more than that.
/// </summary>
public sealed record MirrorChyanPackage(string Url, string Version, string UpdateType, long? Size, string? Sha256)
{
    /// <summary>
    /// True when MirrorChyan will send the whole program archive rather than a difference. That is worth
    /// confirming with the player first: ours is close to a gigabyte.
    /// </summary>
    public bool IsWholePackage => UpdateType == MirrorChyanChannel.FullPackage;
}

/// <summary>
/// The MirrorChyan side of updating: URL shape, response shape, error codes and version normalization.
///
/// Everything here is pure so it can be tested without a network, and everything here is *optional*:
/// MirrorChyan is a second way to learn about and fetch a release, never the authority for one. The
/// signed catalog decides what a version contains; this only says where a copy might be.
/// </summary>
public static class MirrorChyanChannel
{
    public const string ResourceId = "IMAO";
    public const string FullPackage = "full";
    public const string IncrementalPackage = "incremental";

    // The resource is registered per platform, so these are not optional. Verified against the live
    // resource on 2026-09-25: a request without os/arch is refused with 8001, with them it answers 200 -
    // and that holds for the free check that carries no CDK as well. os=windows&arch=amd64 also works,
    // and the server normalizes the answer to those longer spellings.
    public const string Os = "win";
    public const string Arch = "x64";
    public const string Channel = "stable";

    /// <summary>
    /// Identifies this client in MirrorChyan's statistics. <c>mirrorchyan_web</c> is reserved for its own
    /// site and must not be used here.
    /// </summary>
    public const string UserAgent = "IMAO_APP";

    public static readonly Uri Endpoint = new($"https://mirrorchyan.com/api/resources/{ResourceId}/latest");

    /// <summary>MirrorChyan spells "no release notes here" as this literal string.</summary>
    private const string PlaceholderNote = "placeholder";

    /// <summary>
    /// Builds the check request. <paramref name="currentVersion"/> is the program version in the same
    /// spelling the publishing side uploads as <c>version_name</c>, which is the git tag: passing it is
    /// what lets MirrorChyan answer with an incremental package, and omitting it makes MirrorChyan treat
    /// the caller as a fresh installation and answer with a whole package.
    /// <paramref name="cdk"/> is optional; without it the check still works and simply carries no
    /// download URL.
    /// </summary>
    public static Uri BuildRequestUri(string? cdk, string? currentVersion)
    {
        var query = new List<string>(6);
        if (!string.IsNullOrEmpty(currentVersion)) query.Add("current_version=" + Uri.EscapeDataString(currentVersion));
        if (!string.IsNullOrEmpty(cdk)) query.Add("cdk=" + Uri.EscapeDataString(cdk));
        query.Add("user_agent=" + UserAgent);
        query.Add("os=" + Os);
        query.Add("arch=" + Arch);
        query.Add("channel=" + Channel);
        return new Uri(Endpoint.AbsoluteUri + "?" + string.Join('&', query));
    }

    /// <summary>
    /// The request URL with its credential removed. Log this and never the URL itself: the CDK travels in
    /// the query string, which is exactly the part that ends up in logs and crash reports.
    /// </summary>
    public static string Describe(Uri request)
    {
        var text = request.AbsoluteUri;
        var start = text.IndexOf("cdk=", StringComparison.Ordinal);
        if (start < 0) return text;
        var end = text.IndexOf('&', start);
        return text[..start] + "cdk=***" + (end < 0 ? "" : text[end..]);
    }

    /// <summary>
    /// Turns one response body into a result. Never throws: a body this client cannot use is reported as
    /// <see cref="MirrorChyanError.Malformed"/>, because a broken second channel must not be able to fail
    /// the update check that the signed catalog already answered.
    /// </summary>
    /// <param name="programVersion">The running program version, used only to decide <see cref="MirrorChyanResult.HasUpdate"/>.</param>
    public static MirrorChyanResult Parse(string json, string programVersion)
    {
        Payload? payload;
        MirrorChyanError error;
        string message;
        try
        {
            var response = JsonSerializer.Deserialize<Response>(json, UpdateJson.Options);
            if (response?.Code is not int code) return Malformed("Mirror酱 的响应缺少状态码。");
            error = ToError(code);
            message = response.Msg ?? "";
            payload = response.Data;
            if (error == MirrorChyanError.None && payload is null) return Malformed("Mirror酱 的响应缺少内容。");
        }
        catch (JsonException) { return Malformed("Mirror酱 的响应不是有效的 JSON。"); }

        var version = NormalizeVersion(payload?.VersionName);
        var result = new MirrorChyanResult
        {
            Error = error,
            Message = error == MirrorChyanError.None ? message : Describe(error, message),
            Version = version,
            ReleaseNote = NormalizeNote(payload?.ReleaseNote),
            DownloadUrl = string.IsNullOrEmpty(payload?.Url) ? null : payload!.Url,
            Sha256 = string.IsNullOrEmpty(payload?.Sha256) ? null : payload!.Sha256,
            Size = payload?.Filesize,
            UpdateType = payload?.UpdateType ?? "",
            CdkExpiresAt = payload?.CdkExpiredTime is long seconds and > 0 ? DateTimeOffset.FromUnixTimeSeconds(seconds) : null,
        };
        if (error != MirrorChyanError.None) return result;
        if (version.Length == 0) return result with { Error = MirrorChyanError.Malformed, Message = "Mirror酱 返回的版本号无法与本地比较。" };
        return result with { HasUpdate = Compare(version, programVersion) > 0 };
    }

    /// <summary>Builds the result for "the request never produced an answer", which is not a server error code.</summary>
    public static MirrorChyanResult Unreachable(string reason) => new()
    {
        Error = MirrorChyanError.Unreachable,
        Message = "Mirror酱 暂时无法访问：" + reason,
    };

    /// <summary>
    /// Drops the tag's leading <c>v</c> so it can be compared with the program version, and validates that
    /// what is left is the four-part numeric form this program uses. Anything else yields an empty string,
    /// which the caller treats as "no usable version" rather than as an update.
    /// </summary>
    public static string NormalizeVersion(string? versionName)
    {
        if (string.IsNullOrWhiteSpace(versionName)) return "";
        var text = versionName.Trim();
        if (text.Length > 1 && (text[0] == 'v' || text[0] == 'V')) text = text[1..];
        try { UpdateSignature.RequireVersion(text); return text; }
        catch (InvalidDataException) { return ""; }
    }

    /// <summary>Compares two four-part versions. Both must already be normalized.</summary>
    public static int Compare(string left, string right) => UpdateSignature.RequireVersion(left).CompareTo(UpdateSignature.RequireVersion(right));

    /// <summary>MirrorChyan answers "already current" with the literal <c>placeholder</c>, which is not a note.</summary>
    private static string NormalizeNote(string? note)
    {
        var text = (note ?? "").Trim();
        return text.Equals(PlaceholderNote, StringComparison.OrdinalIgnoreCase) ? "" : text;
    }

    private static MirrorChyanResult Malformed(string message) => new() { Error = MirrorChyanError.Malformed, Message = message };

    private static MirrorChyanError ToError(int code) => code switch
    {
        0 => MirrorChyanError.None,
        1 => MirrorChyanError.Undivided,
        1001 => MirrorChyanError.InvalidParams,
        7001 => MirrorChyanError.KeyExpired,
        7002 => MirrorChyanError.KeyInvalid,
        7003 => MirrorChyanError.QuotaExhausted,
        7004 => MirrorChyanError.KeyMismatched,
        7005 => MirrorChyanError.KeyBlocked,
        8001 => MirrorChyanError.ResourceNotFound,
        8002 => MirrorChyanError.InvalidOs,
        8003 => MirrorChyanError.InvalidArch,
        8004 => MirrorChyanError.InvalidChannel,
        < 0 => MirrorChyanError.Unexpected,
        _ => MirrorChyanError.Unexpected,
    };

    /// <summary>
    /// Player-facing wording per code. The server's own <c>msg</c> is English and written for developers,
    /// so it is only shown where this client has nothing better to say.
    /// </summary>
    private static string Describe(MirrorChyanError error, string serverMessage) => error switch
    {
        MirrorChyanError.KeyExpired => "Mirror酱 CDK 已过期，请在设置中更换或前往续费。",
        MirrorChyanError.KeyInvalid => "Mirror酱 CDK 不正确，请在设置中检查。",
        MirrorChyanError.QuotaExhausted => "Mirror酱 CDK 今日下载次数已达上限，请明日再试。",
        MirrorChyanError.KeyMismatched => "这个 Mirror酱 CDK 不适用于本应用。",
        MirrorChyanError.KeyBlocked => "Mirror酱 CDK 已被封禁，请联系 Mirror酱 售后。",
        MirrorChyanError.ResourceNotFound => "Mirror酱 上暂时没有适用于当前平台的版本。",
        MirrorChyanError.InvalidParams or MirrorChyanError.InvalidOs or MirrorChyanError.InvalidArch or MirrorChyanError.InvalidChannel =>
            "Mirror酱 不接受这次请求的参数，这属于程序问题，请反馈给维护者。",
        MirrorChyanError.Unexpected => "Mirror酱 报告了一个严重错误，请联系 Mirror酱 技术支持。" + Suffix(serverMessage),
        _ => serverMessage.Length > 0 ? serverMessage : "Mirror酱 返回了未知错误。",
    };

    private static string Suffix(string serverMessage) => serverMessage.Length > 0 ? "（" + serverMessage + "）" : "";

    // MirrorChyan answers in snake_case while UpdateJson.Options is configured for camelCase, so every
    // field is named explicitly. Leaving one to convention silently binds it to null, and a check that
    // "answered" but carried nothing looks exactly like a server that returned no version.
    private sealed class Response
    {
        [JsonPropertyName("code")] public int? Code { get; set; }
        [JsonPropertyName("msg")] public string? Msg { get; set; }
        [JsonPropertyName("data")] public Payload? Data { get; set; }
    }

    private sealed class Payload
    {
        [JsonPropertyName("version_name")] public string? VersionName { get; set; }
        [JsonPropertyName("version_number")] public long? VersionNumber { get; set; }
        [JsonPropertyName("url")] public string? Url { get; set; }
        [JsonPropertyName("sha256")] public string? Sha256 { get; set; }
        [JsonPropertyName("channel")] public string? Channel { get; set; }
        [JsonPropertyName("os")] public string? Os { get; set; }
        [JsonPropertyName("arch")] public string? Arch { get; set; }
        [JsonPropertyName("update_type")] public string? UpdateType { get; set; }
        [JsonPropertyName("release_note")] public string? ReleaseNote { get; set; }
        [JsonPropertyName("filesize")] public long? Filesize { get; set; }
        [JsonPropertyName("cdk_expired_time")] public long? CdkExpiredTime { get; set; }
    }
}
