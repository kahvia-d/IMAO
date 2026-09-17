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
                value = Migrate(value, HasStoredSchemaVersion(text));
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

    // A stored capture method of 0 came from the version 1 default rather than from a deliberate
    // choice, so a file written before the schema field existed is promoted once: Windows Graphics
    // Capture leaves the game's own presentation path alone, and a machine where it cannot start falls
    // back to BitBlt by itself. The setting still offers both methods, and a player who picks BitBlt in
    // it is recorded with the current schema version and never migrated again.
    private static RuntimeConfiguration Migrate(RuntimeConfiguration value, bool writtenWithSchemaVersion) =>
        writtenWithSchemaVersion ? value : value with { CaptureWay = 1 };

    // The stored field is the only witness for the schema: a deserialized object cannot distinguish a
    // missing property from the default its initializer supplies.
    private static bool HasStoredSchemaVersion(string text)
    {
        using var document = JsonDocument.Parse(text);
        return document.RootElement.ValueKind == JsonValueKind.Object &&
            document.RootElement.TryGetProperty(nameof(RuntimeConfiguration.ConfigVersion), out _);
    }
}
