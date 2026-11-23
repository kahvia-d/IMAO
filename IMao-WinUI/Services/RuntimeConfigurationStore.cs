using System.Text.Json;
using IMao_WinUI.Core.Helpers;
using IMao_WinUI.Models;

namespace IMao_WinUI.Services;

// The service owns updates; readers receive immutable values and never a live dictionary.
internal sealed class RuntimeConfigurationStore
{
    private readonly object gate = new();
    private readonly string path;
    private RuntimeConfiguration? current;
    private bool writable = true;
    public string LoadError { get; private set; } = string.Empty;

    public RuntimeConfigurationStore(string path) => this.path = path;

    public RuntimeConfiguration Read()
    {
        lock (gate)
        {
            if (current is not null) return current;
            try
            {
                var text = File.ReadAllText(path);
                using var document = JsonDocument.Parse(text);
                var root = document.RootElement;
                var value = root.Deserialize<RuntimeConfiguration>()
                    ?? throw new JsonException("配置必须是有效对象");
                // The experimental combined switch also controlled coordinate OCR.
                if (value.OcrGpuAcceleration is null && root.TryGetProperty("GpuAcceleration", out var combined) &&
                    combined.ValueKind is JsonValueKind.True or JsonValueKind.False)
                    value = value with { OcrGpuAcceleration = combined.GetBoolean() };
                var hasStoredPerformanceProfile = root.TryGetProperty(nameof(RuntimeConfiguration.PerformanceProfile), out _);
                value = Migrate(value, StoredSchemaVersion(root), hasStoredPerformanceProfile);
                value.Validate();
                // The file loaded: whatever a migration wanted to say about it is settled, so do not
                // leave a note behind for a file that turned out to be fine.
                LoadError = string.Empty;
                return current = value;
            }
            catch (FileNotFoundException) { return current = new(); }
            catch (DirectoryNotFoundException) { return current = new(); }
            catch (Exception exception) when (exception is JsonException or ArgumentException)
            {
                try { File.Copy(path, path + ".corrupt-" + Guid.NewGuid().ToString("N")); }
                catch (Exception backupError) when (backupError is IOException or UnauthorizedAccessException)
                {
                    writable = false;
                    LoadError = "无法备份损坏的运行配置；修复文件权限后请重启：" + backupError.Message;
                    return current = new();
                }
                LoadError = "运行配置损坏，已保留原始副本并恢复默认值：" + exception.Message;
                return current = new();
            }
            catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
            {
                writable = false;
                LoadError = "无法读取运行配置；修复文件权限后请重启：" + exception.Message;
                return current = new();
            }
        }
    }

    public RuntimeConfiguration Update(Func<RuntimeConfiguration, RuntimeConfiguration> change)
    {
        lock (gate)
        {
            var next = change(Read());
            if (!writable) throw new IOException(LoadError);
            // 落盘时始终记录当前配置版本号，避免下次启动重复执行历史版本迁移。
            if (next.ConfigVersion < RuntimeConfiguration.CurrentSchemaVersion)
                next = next with { ConfigVersion = RuntimeConfiguration.CurrentSchemaVersion };
            next.Validate();
            if (next == current) return next;
            AtomicFile.WriteAllText(path, JsonSerializer.Serialize(next));
            return current = next;
        }
    }

    // A stored value that came from an older default rather than from a deliberate choice is promoted
    // once, when the default changes meaning:
    //
    //   version 2 promoted the capture method. A stored 0 came from the version 1 default, and Windows
    //   Graphics Capture leaves the game's own presentation path alone, so a file without the schema
    //   field is moved to 1.
    //
    //   version 3 promoted the overlay presentation to DirectComposition on the strength of a frame-rate
    //   comparison that could not support it: the two conditions were measured in two consecutive time
    //   windows on a machine whose drift is as large as the effect, and the composition window that was
    //   measured did not carry the WS_EX_LAYERED flag the shipped one has. Version 4 therefore moves
    //   every file written before it back to the layered window. A stored 1 could be that promotion or a
    //   deliberate choice and the two cannot be told apart, so the default wins; a player who wants the
    //   composition surface selects it again and is recorded with version 4 from then on.
    //
    //   version 5 introduced the guide skip key (default G). A file written before it has no such field,
    //   so the deserializer supplies our default rather than the player's choice. If that default lands
    //   on a key they had already bound, the duplicate check below would fail and the whole file would be
    //   backed up as corrupt — every setting reset because we added a key. Disable the new default in
    //   that one case instead. See Migrate for what stays a genuine error.
    //
    // After any migration the file carries the current version, so a player who chooses the other value
    // afterwards is never migrated again.
    private static RuntimeConfiguration Migrate(RuntimeConfiguration value, int storedVersion, bool hasStoredPerformanceProfile)
    {
        var migrated = value;
        if (storedVersion < 2) migrated = migrated with { CaptureWay = 1 };
        if (storedVersion < 4) migrated = migrated with { OverlayPresentMode = 0 };
        // A file that predates the guide skip key cannot have chosen its default, so a collision with
        // one of its own bindings is our problem, not a corrupt file. Only that field is dropped: if the
        // remaining set is still inconsistent, the file really is damaged and stays an error.
        if (storedVersion < 5 && migrated.GuideSkipKey != 0 &&
            new[]
            {
                migrated.NearestCompletionKey, migrated.ManualRouteKey, migrated.CurrentTargetGuideKey,
                migrated.GuidePreviousImageKey, migrated.GuideNextImageKey, migrated.ToggleEnabledKey
            }.Contains(migrated.GuideSkipKey))
        {
            migrated = migrated with { GuideSkipKey = 0 };
        }
        // 版本 6 引入性能与刷新档位：仅对确实不含 PerformanceProfile 字段的旧配置按刷新间隔推断；
        // 若文件已显式记录 PerformanceProfile（如旧迁移未抬升版本号导致的遗留文件），保留用户已选档位。
        if (storedVersion < 6)
        {
            if (!hasStoredPerformanceProfile)
            {
                migrated = migrated with
                {
                    PerformanceProfile = (migrated.MapUpdateCycle != 80 || migrated.MinMapUpdateCycle != 80) ? 2 : 1
                };
            }
        }
        // 版本 7 精简为三档（0=流畅，1=均衡，2=自定义）：旧 v6 的低占用（2）迁至均衡（1），自定义（3）迁至（2）。
        else if (storedVersion < 7)
        {
            migrated = migrated with
            {
                PerformanceProfile = migrated.PerformanceProfile switch
                {
                    2 => 1,
                    3 => 2,
                    _ => migrated.PerformanceProfile
                }
            };
        }
        // 版本 8 将默认叠加层呈现方式切换为 DirectComposition（1）；旧版本配置一次性迁至新默认，
        // 之后用户在版本 8 手动切回分层窗口（0）时会持久化保留，不再重复迁移。
        if (storedVersion < 8)
        {
            migrated = migrated with { OverlayPresentMode = 1 };
        }
        // 迁移完成后将配置版本号升至当前版本，确保后续保存与重启不再重复触发旧版迁移。
        if (migrated.ConfigVersion < RuntimeConfiguration.CurrentSchemaVersion)
        {
            migrated = migrated with { ConfigVersion = RuntimeConfiguration.CurrentSchemaVersion };
        }
        return migrated;
    }

    // The stored field is the only witness for the schema: a deserialized object cannot distinguish a
    // missing property from the default its initializer supplies. A file with no version at all is
    // treated as the oldest schema, which is what it is.
    private static int StoredSchemaVersion(JsonElement root)
    {
        return root.TryGetProperty(nameof(RuntimeConfiguration.ConfigVersion), out var version) &&
            version.TryGetInt32(out var value) ? value : 0;
    }
}
