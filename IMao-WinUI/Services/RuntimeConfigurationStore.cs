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
                var value = JsonSerializer.Deserialize<RuntimeConfiguration>(File.ReadAllText(path))
                    ?? throw new JsonException("配置必须是有效对象");
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
}
