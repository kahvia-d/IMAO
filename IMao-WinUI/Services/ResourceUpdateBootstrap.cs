using System.Diagnostics;
using System.Text.Json;
using IMao_WinUI.Core.Updates;
using IMao_WinUI.Helpers;

namespace IMao_WinUI.Services;

internal static class ResourceUpdateBootstrap
{
    public static BuildInfo ReadBuildInfo()
    {
        string path = Path.Combine(AppContext.BaseDirectory, "build-info.json");
        return File.Exists(path) ? JsonSerializer.Deserialize<BuildInfo>(File.ReadAllText(path), UpdateJson.Options)
            ?? throw new InvalidDataException("程序版本信息无效。") : new BuildInfo();
    }

    public static ResourceSnapshotService CreateSnapshots()
    {
        var info = ReadBuildInfo();
        string assets = Path.Combine(AppContext.BaseDirectory, "Assets");
        string descriptor = Path.Combine(assets, "Updates", "bundled-snapshot.json");
        ResourceSnapshot bundled;
        if (File.Exists(descriptor))
        {
            var relative = JsonSerializer.Deserialize<ResourceSnapshot>(File.ReadAllText(descriptor), UpdateJson.Options)
                ?? throw new InvalidDataException("内置资源清单无效。");
            if (!relative.Bundled || relative.FormatVersion != 1 || relative.BaselineId != info.BaselineId)
                throw new InvalidDataException("内置资源与程序版本不一致，请重新安装完整程序包。");
            bundled = relative with
            {
                BaselineRoot = assets,
                MapDataRoot = ResolveBundledPath(assets, relative.MapDataRoot),
                MapIconRoot = string.IsNullOrEmpty(relative.MapIconRoot) ? "" : ResolveBundledPath(assets, relative.MapIconRoot),
                MapFeatureRoot = string.IsNullOrEmpty(relative.MapFeatureRoot) ? "" : ResolveBundledPath(assets, relative.MapFeatureRoot),
                Packages = relative.Packages.Select(p => p with { Directory = ResolveBundledPath(assets, p.Directory) }).ToList()
            };
        }
        else
        {
            // Legacy/development bundles stay usable; production packaging requires the descriptor.
            var packages = new List<SnapshotPackage>();
            string features = Path.Combine(assets, "FeaturesDatas");
            foreach (var kind in new[] { "tile", "candidate" })
            {
                string registry = Path.Combine(features, kind == "tile" ? "kuro-tile-packs.json" : "candidate-packs.json");
                if (!File.Exists(registry)) continue;
                using var document = JsonDocument.Parse(File.ReadAllText(registry));
                foreach (var entry in document.RootElement.GetProperty("packs").EnumerateArray())
                {
                    string name = entry.GetString() ?? "";
                    if (name.Length == 0 || name != Path.GetFileName(name) || name.Contains('.')) continue;
                    string directory = kind == "tile" ? Path.Combine(features, "KuroTilePacks", name) : Path.Combine(features, name);
                    string manifest = Path.Combine(directory, "manifest.json");
                    if (!File.Exists(manifest) || !File.Exists(Path.Combine(directory, "visual-index.imx"))) continue;
                    using var data = JsonDocument.Parse(File.ReadAllText(manifest));
                    if (kind == "tile" && (!data.RootElement.TryGetProperty("referenceVerification", out var reference) ||
                        !reference.TryGetProperty("passed", out var passed) || passed.ValueKind != JsonValueKind.True)) continue;
                    // New independent scenes require an explicitly generated and validated production descriptor.
                    if (data.RootElement.TryGetProperty("scene", out var scene) && scene.GetString() != "World") continue;
                    packages.Add(new() { Id = name, Version = "bundled", Kind = kind, Directory = directory });
                }
            }
            bundled = new() { SnapshotId = "bundled-" + info.AppVersion, BaselineId = info.BaselineId,
                BaselineRoot = assets, MapDataRoot = Path.Combine(assets, "KuroMap"), Bundled = true, Packages = packages };
        }
        return new ResourceSnapshotService(Path.Combine(UserDataPaths.Root, "ResourceUpdates"), bundled, info.AppVersion, PreflightAsync, Path.Combine(AppContext.BaseDirectory, "IMao-CoreHost.exe"));
    }

    public static UpdateService CreateUpdater(ResourceSnapshotService snapshots)
    {
        string path = Path.Combine(AppContext.BaseDirectory, "Assets", "Updates", "trusted-keys.json");
        TrustedUpdateKeys keys = new();
        string? initializationError = null;
        try
        {
            keys = JsonSerializer.Deserialize<TrustedUpdateKeys>(File.ReadAllText(path), UpdateJson.Options)
                ?? throw new InvalidDataException("更新公钥清单无效。");
            if (keys.Keys is null || keys.Keys.Count == 0) throw new InvalidDataException("未配置可信发布公钥。");
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException)
        {
            keys = new();
            initializationError = "更新功能暂不可用，请重新安装完整程序包：" + error.Message;
        }
        return new UpdateService(ReadBuildInfo(), keys.Keys, snapshots, initializationError: initializationError);
    }

    private static string ResolveBundledPath(string assets, string relative)
    {
        if (Path.IsPathRooted(relative)) throw new InvalidDataException("内置资源路径必须为相对路径。");
        string result = Path.GetFullPath(Path.Combine(assets, relative));
        if (!result.StartsWith(Path.GetFullPath(assets) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("内置资源路径越界。");
        return result;
    }

    private static async Task PreflightAsync(string snapshotPath, CancellationToken cancellationToken)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeout.CancelAfter(TimeSpan.FromMinutes(5));
        var checkToken = timeout.Token;
        string expectedId = JsonSerializer.Deserialize<ResourceSnapshot>(await File.ReadAllTextAsync(snapshotPath, checkToken), UpdateJson.Options)?.SnapshotId
            ?? throw new InvalidDataException("候选资源快照无效。");
        string exe = Path.Combine(AppContext.BaseDirectory, "IMao-CoreHost.exe");
        using var process = new Process { StartInfo = new ProcessStartInfo(exe)
        {
            WorkingDirectory = AppContext.BaseDirectory, UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true
        } };
        process.StartInfo.ArgumentList.Add("--check-resource-snapshot");
        process.StartInfo.ArgumentList.Add(snapshotPath);
        if (!process.Start()) throw new IOException("无法启动资源验证进程。");
        using var registration = checkToken.Register(() => { try { if (!process.HasExited) process.Kill(entireProcessTree: true); } catch (InvalidOperationException) { } });
        Task<string> output = process.StandardOutput.ReadToEndAsync(checkToken);
        Task<string> error = process.StandardError.ReadToEndAsync(checkToken);
        try { await process.WaitForExitAsync(checkToken).ConfigureAwait(false); }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        { throw new TimeoutException("资源预检超过五分钟，请检查磁盘状态后重试。"); }
        string report = await output.ConfigureAwait(false), diagnostic = await error.ConfigureAwait(false);
        if (process.ExitCode != 0)
            throw new InvalidDataException("候选地图资源未通过完整验证：" + (report + " " + diagnostic).Trim());
        // Require a positive machine-readable response, not only process exit status.
        bool ready = false;
        foreach (string line in report.Split('\n', StringSplitOptions.RemoveEmptyEntries))
        {
            try
            {
                using var json = JsonDocument.Parse(line);
                ready |= json.RootElement.TryGetProperty("resourcesReady", out var value) && value.ValueKind == JsonValueKind.True &&
                    json.RootElement.TryGetProperty("resourceSnapshotId", out var id) && id.GetString() == expectedId;
            }
            catch (JsonException) { }
        }
        if (!ready) throw new InvalidDataException("资源验证进程未返回就绪确认。");
    }
}
