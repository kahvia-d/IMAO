using System.Text.Json;

namespace IMao_WinUI.Helpers;

internal sealed class RegionalProgressReader
{
    internal sealed record Snapshot(string? Path, IReadOnlySet<string> CompletedKeys);
    private readonly object gate = new();
    private Snapshot? cached;
    private (DateTime Written, DateTime Created, long Length)? cachedStamp;

    // Both metadata probes and JSON parsing run off the caller's (usually UI) thread.
    // Only publish a cache entry after a successful read; failures must remain retryable.
    public Task<Snapshot> ReadAsync(string? path, bool force = false) => Task.Run(() =>
    {
        lock (gate) return Read(path, force);
    });

    private Snapshot Read(string? path, bool force)
    {
        (DateTime Written, DateTime Created, long Length)? stamp = null;
        if (path is not null)
        {
            try
            {
                var info = new FileInfo(path);
                stamp = (info.LastWriteTimeUtc, info.CreationTimeUtc, info.Length);
            }
            catch (FileNotFoundException) { }
            catch (DirectoryNotFoundException) { }
        }
        bool samePath = cached is not null && string.Equals(cached.Path, path, StringComparison.OrdinalIgnoreCase);
        if (!force && samePath && stamp == cachedStamp) return cached!;

        var keys = new HashSet<string>(StringComparer.Ordinal);
        if (stamp is not null)
        {
            using var stream = new FileStream(path!, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            using var doc = JsonDocument.Parse(stream);
            if (doc.RootElement.ValueKind != JsonValueKind.Object ||
                !doc.RootElement.TryGetProperty("points", out var points) || points.ValueKind != JsonValueKind.Object)
                throw new JsonException("进度文件缺少有效的点位数据。");
            foreach (var point in points.EnumerateObject())
                if (point.Value.ValueKind == JsonValueKind.Object &&
                    point.Value.TryGetProperty("completed", out var flag) && flag.ValueKind == JsonValueKind.True)
                    keys.Add(point.Name);
        }
        cachedStamp = stamp;
        // Compare identities, not just counts: sync may move completion between regions.
        if (!samePath || !cached!.CompletedKeys.SetEquals(keys)) cached = new Snapshot(path, keys);
        return cached!;
    }
}
