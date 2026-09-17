using System.Reflection;
using System.Diagnostics;
using System.Security.Principal;
using System.Runtime.InteropServices;
using System.Text.Json;
using IMao_WinUI.Core.Updates;

internal static class EntryPoint
{
    static async Task<int> Main(string[] args)
    {
        try
        {
            using var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("trusted-keys.json")!;
            var keys = (await JsonSerializer.DeserializeAsync<TrustedUpdateKeys>(stream, UpdateJson.Options))!.Keys;
            if (keys.Count == 0 || keys.Any(k => k.TestOnly)) throw new InvalidDataException("启动器必须内置正式发布公钥。");
            if (args.SequenceEqual(new[] { "--self-check" })) return 0;
            if (args.Length != 0) throw new ArgumentException("启动器不接受安装路径、外部公钥或执行命令参数。");
            if (OperatingSystem.IsWindows() && !new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator))
            {
                using var elevated = Process.Start(new ProcessStartInfo(Environment.ProcessPath!) { UseShellExecute = true, Verb = "runas", WorkingDirectory = AppContext.BaseDirectory });
                return elevated is null ? 1 : 0;
            }
            var store = new ProgramUpdateStore(AppContext.BaseDirectory, keys, ReadRunningVersion());
            await new ProgramLauncher(store).RunAsync();
            return 0;
        }
        catch (Exception error)
        {
            MessageBox(IntPtr.Zero, error.Message, "IMao 程序启动与更新", 0x10);
            return 1;
        }
    }

    // The launcher only starts installed versions and never prepares an update. The running version is
    // still reported so that a future prepare can never re-sync the sequence record from an older
    // catalog; an unreadable descriptor keeps the strict rule instead of blocking startup.
    static string ReadRunningVersion()
    {
        try
        {
            var path = Path.Combine(AppContext.BaseDirectory, "build-info.json");
            return File.Exists(path)
                ? JsonSerializer.Deserialize<BuildInfo>(File.ReadAllText(path), UpdateJson.Options)?.AppVersion ?? ""
                : "";
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException) { return ""; }
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int MessageBox(IntPtr window, string text, string caption, uint type);
}
