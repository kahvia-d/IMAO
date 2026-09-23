using IMao_WinUI.Models;
using System.Collections.Concurrent;
using System.Net;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace IMao_WinUI.Services;

// Public guide content only. This client deliberately has no account cookies or authorization headers.
public sealed class MarkerDetailService : IDisposable
{
    private const int MaxDetailBytes = 2 * 1024 * 1024;
    private const int MaxImageBytes = 12 * 1024 * 1024;
    private static readonly TimeSpan CacheAge = TimeSpan.FromHours(24);
    private readonly string mapDirectory;
    private readonly string cacheDirectory;
    private readonly HttpClient http;
    private readonly bool ownsClient;
    private readonly ConcurrentDictionary<int, Lazy<Task<IReadOnlyDictionary<string, MarkerDetail>>>> localStates = new();
    private readonly SemaphoreSlim cacheLock = new(1, 1);

    public MarkerDetailService() : this(IMao_WinUI.Helpers.ResourceSessionPaths.MapDataRoot,
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "IMao-WinUI", "GuideCache")) { }

    public MarkerDetailService(string mapDirectory, string cacheDirectory, HttpClient? httpClient = null)
    {
        this.mapDirectory = Path.GetFullPath(mapDirectory);
        this.cacheDirectory = Path.GetFullPath(cacheDirectory);
        ownsClient = httpClient is null;
        http = httpClient ?? new HttpClient(new HttpClientHandler { AllowAutoRedirect = false, UseCookies = false })
        { Timeout = TimeSpan.FromSeconds(20) };
    }

    public async Task<MarkerDetail> GetLocalAsync(MarkerSelection selection, CancellationToken cancellationToken = default)
    {
        ValidateSelection(selection);
        var index = await localStates.GetOrAdd(selection.StateId, state => new(() => Task.Run(() => LoadLocalState(state))))
            .Value.WaitAsync(cancellationToken).ConfigureAwait(false);
        if (index.TryGetValue(selection.PointId, out var detail)) return detail;
        return new MarkerDetail
        {
            PointId = selection.PointId, StateId = selection.StateId, CountryId = selection.CountryId,
            TypeId = OfficialTypeId(selection.NameId), Name = selection.NameId,
            FloorId = selection.FloorId, Level = selection.Level, SourceUrl = BuildSourceUrl(selection)
        };
    }

    // The caller chooses approved states. Loading a catalog does not enable its scene in the game.
    public async Task<IReadOnlyList<MarkerSelection>> GetStateMarkersAsync(int stateId, CancellationToken cancellationToken = default)
    {
        if (stateId <= 0) throw new ArgumentOutOfRangeException(nameof(stateId));
        var index = await localStates.GetOrAdd(stateId, state => new(() => Task.Run(() => LoadLocalState(state))))
            .Value.WaitAsync(cancellationToken).ConfigureAwait(false);
        return index.Values.Select(detail => new MarkerSelection
        {
            Scene = stateId switch { 8 => "World", 900 => "Tethys", 905 => "Fabricatorium", 903 => "Avinoleum",
                906 => "Lahai", 902 => "LowerVault", 909 => "Darkplain", 910 => "TimeRiftRuins", _ => string.Empty },
            NameId = detail.TypeId switch { "sx·qq" => "sx_qq", "sx·lgn" => "sx_lgn", _ => detail.TypeId },
            PointId = detail.PointId, StateId = stateId, CountryId = detail.CountryId,
            FloorId = detail.FloorId, Level = detail.Level
        }).ToArray();
    }

    public async Task<MarkerDetailResult> GetOnlineAsync(MarkerSelection selection, MarkerDetail local,
        CancellationToken cancellationToken = default, bool refresh = false)
    {
        ValidateSelection(selection);
        var cached = await ReadCacheAsync(selection, local, cancellationToken).ConfigureAwait(false);
        if (!refresh && cached is not null && DateTimeOffset.UtcNow - cached.Value.SavedAt < CacheAge)
            return new(cached.Value.Detail, "已加载缓存攻略", true);
        try
        {
            using var request = new HttpRequestMessage(HttpMethod.Post, "https://api.kurobbs.com/map/core/position/getDetailOnline");
            request.Headers.Add("source", "h5");
            request.Headers.Add("wiki_type", "10");
            request.Headers.Add("state_id", selection.StateId.ToString(System.Globalization.CultureInfo.InvariantCulture));
            request.Content = new FormUrlEncodedContent(new Dictionary<string, string> { ["id"] = selection.PointId });
            using var response = await http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, cancellationToken).ConfigureAwait(false);
            response.EnsureSuccessStatusCode();
            var bytes = await ReadBoundedAsync(response.Content, MaxDetailBytes, cancellationToken).ConfigureAwait(false);
            using var document = JsonDocument.Parse(bytes);
            var detail = ParseOnline(document.RootElement, selection, local);
            string status = "攻略来源：库街区";
            try
            {
                await WriteCacheAsync(CachePath(selection), JsonSerializer.SerializeToUtf8Bytes(new
                { savedAt = DateTimeOffset.UtcNow, response = document.RootElement }), cancellationToken).ConfigureAwait(false);
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { status = "攻略已加载，缓存暂不可写"; }
            return new(detail, status);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested) { throw; }
        catch (Exception ex) when (ex is HttpRequestException or IOException or JsonException or InvalidDataException or InvalidOperationException or OperationCanceledException)
        {
            return cached is not null
                ? new(cached.Value.Detail, "暂时无法更新，正在显示缓存攻略", true)
                : new(local, "暂时无法加载在线攻略，正在显示本地说明");
        }
    }

    public async Task<string> GetPicturePathAsync(string url, CancellationToken cancellationToken = default)
    {
        if (!TryGetPictureUri(url, out var uri)) throw new InvalidDataException("攻略图片地址无效");
        string path = Path.Combine(cacheDirectory, "images", Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(uri.AbsoluteUri))) + ".image");
        var info = new FileInfo(path);
        if (info.Exists && info.Length > 0 && info.Length <= MaxImageBytes && DateTime.UtcNow - info.LastWriteTimeUtc < TimeSpan.FromDays(30))
            return path;
        using var response = await http.GetAsync(uri, HttpCompletionOption.ResponseHeadersRead, cancellationToken).ConfigureAwait(false);
        response.EnsureSuccessStatusCode();
        // Production clients disable redirects; injected handlers are still checked before their response is used.
        if (response.RequestMessage?.RequestUri is Uri finalUri && !TryGetPictureUri(finalUri.AbsoluteUri, out _))
            throw new InvalidDataException("攻略图片重定向地址无效");
        string? contentType = response.Content.Headers.ContentType?.MediaType;
        if (contentType is not ("image/png" or "image/jpeg" or "image/webp" or "image/gif"))
            throw new InvalidDataException("攻略图片格式不支持");
        byte[] bytes = await ReadBoundedAsync(response.Content, MaxImageBytes, cancellationToken).ConfigureAwait(false);
        if (!HasImageSignature(bytes)) throw new InvalidDataException("攻略图片内容无效");
        await WriteCacheAsync(path, bytes, cancellationToken).ConfigureAwait(false);
        await TrimImageCacheAsync(cancellationToken).ConfigureAwait(false);
        return path;
    }

    public static bool TryGetPictureUri(string? value, out Uri uri)
    {
        if (TryGetExternalUri(value, out uri) &&
            (uri.IdnHost.Equals("kurobbs.com", StringComparison.OrdinalIgnoreCase) ||
             uri.IdnHost.EndsWith(".kurobbs.com", StringComparison.OrdinalIgnoreCase))) return true;
        uri = null!;
        return false;
    }

    public static bool TryGetExternalUri(string? value, out Uri uri)
    {
        if (Uri.TryCreate(value, UriKind.Absolute, out var parsed) && parsed.Scheme == Uri.UriSchemeHttps &&
            parsed.UserInfo.Length == 0 && parsed.IsDefaultPort && !parsed.IsLoopback &&
            parsed.HostNameType == UriHostNameType.Dns && parsed.Host.Contains('.') &&
            !parsed.Host.EndsWith(".local", StringComparison.OrdinalIgnoreCase) &&
            !parsed.Host.EndsWith(".localhost", StringComparison.OrdinalIgnoreCase))
        { uri = parsed; return true; }
        uri = null!;
        return false;
    }

    public static string BuildSourceUrl(MarkerSelection selection) =>
        $"https://www.kurobbs.com/mc/map/?state={selection.StateId}&country={selection.CountryId}&typeId={Uri.EscapeDataString(OfficialTypeId(selection.NameId))}&pointId={Uri.EscapeDataString(selection.PointId)}";

    private static string OfficialTypeId(string typeId) => typeId switch { "sx_qq" => "sx·qq", "sx_lgn" => "sx·lgn", _ => typeId };

    private IReadOnlyDictionary<string, MarkerDetail> LoadLocalState(int state)
    {
        var result = new Dictionary<string, MarkerDetail>(StringComparer.Ordinal);
        try
        {
            using var input = File.OpenRead(Path.Combine(mapDirectory, "states", $"state-{state}.json"));
            using var document = JsonDocument.Parse(input);
            foreach (var group in document.RootElement.EnumerateArray())
            {
                if (!group.TryGetProperty("location", out var positions) || positions.ValueKind != JsonValueKind.Array) continue;
                foreach (var point in positions.EnumerateArray())
                {
                    var selection = new MarkerSelection
                    {
                        PointId = Text(point, "id"), StateId = state, CountryId = Integer(point, "countryId"),
                        NameId = Text(group, "id"), FloorId = Text(point, "floorId"), Level = Text(point, "level")
                    };
                    if (selection.PointId.Length == 0) continue;
                    result[selection.PointId] = new MarkerDetail
                    {
                        PointId = selection.PointId, StateId = state, CountryId = selection.CountryId, TypeId = selection.NameId,
                        Name = Text(group, "name"), Description = Text(point, "description"),
                        FloorId = selection.FloorId, Level = selection.Level, SourceUrl = BuildSourceUrl(selection)
                    };
                }
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException or InvalidOperationException) { }
        return result;
    }

    internal static MarkerDetail ParseOnline(JsonElement root, MarkerSelection selection, MarkerDetail local)
    {
        if (!root.TryGetProperty("data", out var data) || data.ValueKind != JsonValueKind.Object ||
            Text(data, "id") != selection.PointId || Integer(data, "stateId") != selection.StateId)
            throw new InvalidDataException("攻略点位与选择不一致");
        if (!data.TryGetProperty("content", out var content) || content.ValueKind != JsonValueKind.Object)
            return local;
        var pictures = new List<string>();
        if (content.TryGetProperty("picturesUrl", out var pictureList) && pictureList.ValueKind == JsonValueKind.Array)
            foreach (var picture in pictureList.EnumerateArray())
                if (picture.ValueKind == JsonValueKind.String && TryGetPictureUri(picture.GetString(), out var uri) && pictures.Count < 40)
                    pictures.Add(uri.AbsoluteUri);
        string? guide = null;
        bool linkVisible = content.TryGetProperty("linkVisible", out var visible) &&
            (visible.ValueKind == JsonValueKind.True || visible.ValueKind == JsonValueKind.Number && visible.TryGetInt32(out int flag) && flag == 1);
        if (linkVisible && content.TryGetProperty("link", out var link) && link.ValueKind == JsonValueKind.Object &&
            TryGetExternalUri(Text(link, "href"), out var guideUri)) guide = guideUri.AbsoluteUri;
        string name = Text(data, "name");
        return local with
        {
            Name = string.IsNullOrWhiteSpace(name) ? local.Name : name,
            Description = Text(content, "description"), PictureUrls = pictures.Distinct().ToArray(), GuideUrl = guide,
            LastUpdateTime = Text(data, "lastUpdateTime")
        };
    }

    private async Task<(DateTimeOffset SavedAt, MarkerDetail Detail)?> ReadCacheAsync(MarkerSelection selection, MarkerDetail local, CancellationToken token)
    {
        try
        {
            string path = CachePath(selection);
            if (!File.Exists(path) || new FileInfo(path).Length > MaxDetailBytes + 1024) return null;
            byte[] bytes = await File.ReadAllBytesAsync(path, token).ConfigureAwait(false);
            using var document = JsonDocument.Parse(bytes);
            var root = document.RootElement;
            if (!root.GetProperty("savedAt").TryGetDateTimeOffset(out var savedAt) || savedAt > DateTimeOffset.UtcNow.AddMinutes(5)) return null;
            return (savedAt, ParseOnline(root.GetProperty("response"), selection, local));
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException or InvalidDataException or KeyNotFoundException or InvalidOperationException) { return null; }
    }

    private string CachePath(MarkerSelection selection) => Path.Combine(cacheDirectory, "details", $"{selection.StateId}-{selection.PointId}.json");

    private async Task WriteCacheAsync(string path, byte[] bytes, CancellationToken token)
    {
        await cacheLock.WaitAsync(token).ConfigureAwait(false);
        string temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            await File.WriteAllBytesAsync(temporary, bytes, token).ConfigureAwait(false);
            token.ThrowIfCancellationRequested();
            File.Move(temporary, path, true);
        }
        finally
        {
            try { if (File.Exists(temporary)) File.Delete(temporary); } catch (IOException) { }
            cacheLock.Release();
        }
    }

    private async Task TrimImageCacheAsync(CancellationToken token)
    {
        await cacheLock.WaitAsync(token).ConfigureAwait(false);
        try
        {
            var files = new DirectoryInfo(Path.Combine(cacheDirectory, "images")).EnumerateFiles("*.image")
                .OrderByDescending(file => file.LastWriteTimeUtc).ToArray();
            long total = 0;
            foreach (var file in files)
            {
                total += file.Length;
                if (total > 256L * 1024 * 1024) try { file.Delete(); } catch (IOException) { }
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
        finally { cacheLock.Release(); }
    }

    private static async Task<byte[]> ReadBoundedAsync(HttpContent content, int maximum, CancellationToken token)
    {
        if (content.Headers.ContentLength > maximum) throw new InvalidDataException("攻略内容过大");
        await using var input = await content.ReadAsStreamAsync(token).ConfigureAwait(false);
        using var output = new MemoryStream();
        byte[] buffer = new byte[32 * 1024];
        int count;
        while ((count = await input.ReadAsync(buffer, token).ConfigureAwait(false)) > 0)
        {
            if (output.Length + count > maximum) throw new InvalidDataException("攻略内容过大");
            await output.WriteAsync(buffer.AsMemory(0, count), token).ConfigureAwait(false);
        }
        return output.ToArray();
    }

    private static bool HasImageSignature(byte[] bytes) =>
        bytes.Length >= 12 && (bytes.AsSpan(0, 8).SequenceEqual(new byte[] { 137, 80, 78, 71, 13, 10, 26, 10 }) ||
        bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff ||
        Encoding.ASCII.GetString(bytes, 0, 3) == "GIF" ||
        Encoding.ASCII.GetString(bytes, 0, 4) == "RIFF" && Encoding.ASCII.GetString(bytes, 8, 4) == "WEBP");

    private static string Text(JsonElement value, string property) => value.TryGetProperty(property, out var field)
        ? field.ValueKind == JsonValueKind.String ? field.GetString() ?? string.Empty : field.ValueKind == JsonValueKind.Number ? field.GetRawText() : string.Empty
        : string.Empty;
    private static int Integer(JsonElement value, string property) => int.TryParse(Text(value, property), out int result) ? result : 0;
    private static void ValidateSelection(MarkerSelection selection)
    {
        if (selection.StateId <= 0 || selection.PointId.Length is < 1 or > 32 || selection.PointId.Any(c => c is < '0' or > '9'))
            throw new ArgumentException("点位编号无效", nameof(selection));
    }

    public void Dispose() { if (ownsClient) http.Dispose(); }
}
