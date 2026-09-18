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
                var value = JsonSerializer.Deserialize<RuntimeConfiguration>(text)
                    ?? throw new JsonException("配置必须是有效对象");
                value = Migrate(value, StoredSchemaVersion(text));
                value.Validate();
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
    //   version 3 promotes the overlay presentation to DirectComposition, which measured about +13 fps
    //   with the frames over 20 ms falling from 17.5% to 1.8%. A stored 0 cannot be told apart from the
    //   old default, so every file written before this bump is moved to it.
    //
    // After either migration the file carries the current version, so a player who chooses the other
    // value afterwards is never migrated again.
    private static RuntimeConfiguration Migrate(RuntimeConfiguration value, int storedVersion)
    {
        var migrated = value;
        if (storedVersion < 2) migrated = migrated with { CaptureWay = 1 };
        if (storedVersion < 3) migrated = migrated with { OverlayPresentMode = 1 };
        return migrated;
    }

    // The stored field is the only witness for the schema: a deserialized object cannot distinguish a
    // missing property from the default its initializer supplies. A file with no version at all is
    // treated as the oldest schema, which is what it is.
    private static int StoredSchemaVersion(string text)
    {
        using var document = JsonDocument.Parse(text);
        if (document.RootElement.ValueKind != JsonValueKind.Object) return 0;
        return document.RootElement.TryGetProperty(nameof(RuntimeConfiguration.ConfigVersion), out var version) &&
            version.TryGetInt32(out var value) ? value : 0;
    }
}
