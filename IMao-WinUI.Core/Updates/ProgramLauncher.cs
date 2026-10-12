#nullable enable
using System.Diagnostics;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Microsoft.Win32.SafeHandles;

namespace IMao_WinUI.Core.Updates;

public sealed record ProgramHealth(string Nonce, int ProcessId, string Version, string ResourceSnapshotId);

public static class ProgramLaunchSession
{
    public static bool IsManaged => !string.IsNullOrEmpty(Environment.GetEnvironmentVariable("IMAO_LAUNCH_PIPE"));
    public static async Task ReportHealthyAsync(string version, string resourceSnapshotId, CancellationToken ct = default)
    {
        var pipe = Environment.GetEnvironmentVariable("IMAO_LAUNCH_PIPE");
        var nonce = Environment.GetEnvironmentVariable("IMAO_LAUNCH_NONCE");
        if (pipe is null || nonce is null) return;
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct); timeout.CancelAfter(TimeSpan.FromSeconds(15));
        await using var client = new NamedPipeClientStream(".", pipe, PipeDirection.InOut, PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
        await client.ConnectAsync(timeout.Token);
        using var writer = new StreamWriter(client, leaveOpen: true) { AutoFlush = true };
        using var reader = new StreamReader(client, leaveOpen: true);
        await writer.WriteLineAsync(JsonSerializer.Serialize(new ProgramHealth(nonce, Environment.ProcessId, version, resourceSnapshotId)).AsMemory(), timeout.Token);
        if (await reader.ReadLineAsync(timeout.Token) != "accepted") throw new IOException("启动器没有确认新版程序就绪。");
    }
}

// The stable launcher is never replaced in place. All activation decisions are durable before Process.Start.
public sealed class ProgramLauncher
{
    private readonly ProgramUpdateStore store;
    private readonly Func<string, ProcessStartInfo> startFactory;
    private readonly TimeSpan healthTimeout;
    /// <summary>
    /// Why the last attempt failed, in the terms a support conversation needs: the child's exit code when it
    /// chose to exit, or the fact that this launcher had to kill it. Empty until an attempt fails.
    /// </summary>
    public string LastFailure { get; private set; } = "";
    public ProgramLauncher(ProgramUpdateStore store, Func<string, ProcessStartInfo>? startFactory = null, TimeSpan? healthTimeout = null)
    {
        this.store = store;
        // Standard error is captured because a child that dies before it can report anything leaves its only
        // explanation there: the .NET apphost and the CLR write startup failures to stderr and exit, which for
        // a windowed process is otherwise invisible.
        this.startFactory = startFactory ?? (directory => new ProcessStartInfo(Path.Combine(directory, "IMao-WinUI.exe")) { WorkingDirectory = directory, UseShellExecute = false, RedirectStandardError = true });
        this.healthTimeout = healthTimeout ?? TimeSpan.FromMinutes(5);
    }

    public async Task RunAsync(CancellationToken ct = default)
    {
        UpdateStorage.RejectLink(store.Root); Directory.CreateDirectory(store.Root);
        FileStream lifetime;
        try { lifetime = new FileStream(Path.Combine(store.Root, "launcher.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None); }
        catch (IOException ex) { throw new IOException("此安装目录的程序已经运行。请在已打开的软件中操作，或完全退出后再试。", ex); }
        await using var lease = lifetime;
        // The child retains this lease even if its launcher crashes. Never activate another version beside it.
        using (new FileStream(Path.Combine(store.Root, "running.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None)) { }
        bool retryFallback = true;
        while (true)
        {
            ct.ThrowIfCancellationRequested();
            var launch = await store.BeginLaunchAsync(ct);
            var directory = store.AppDirectory(launch.Id);
            var build = UpdateStorage.Read<BuildInfo>(Path.Combine(directory, "build-info.json"));
            var name = "IMao.ProgramHealth." + Guid.NewGuid().ToString("N");
            var nonce = Guid.NewGuid().ToString("N");
            using var job = new ProgramProcessJob();
            using var pipe = new NamedPipeServerStream(name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte, PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
            var start = startFactory(directory);
            start.Environment["IMAO_LAUNCH_PIPE"] = name;
            start.Environment["IMAO_LAUNCH_NONCE"] = nonce;
            start.Environment["IMAO_LAUNCH_JOB"] = job.Name;
            start.Environment["IMAO_LAUNCH_TRIAL"] = launch.Trial ? "1" : "0";
            start.Environment["IMAO_LAUNCH_PARENT"] = Environment.ProcessId.ToString();
            start.Environment["IMAO_LAUNCH_PARENT_START"] = Process.GetCurrentProcess().StartTime.ToUniversalTime().Ticks.ToString();
            // Windows environment keys are case-insensitive, unlike some inherited .NET dictionaries.
            var path = Environment.GetEnvironmentVariable("PATH"); start.Environment.Remove("Path"); start.Environment.Remove("PATH"); start.Environment["PATH"] = path;
            using var child = new Process { StartInfo = start };
            Task<string>? errorOutput = null;
            try
            {
                if (!child.Start()) throw new IOException("无法启动程序。");
                if (start.RedirectStandardError) errorOutput = DrainTailAsync(child.StandardError);
                using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct); timeout.CancelAfter(healthTimeout);
                var health = ReadHealthAsync(pipe, child, nonce, build.AppVersion, timeout.Token);
                var exit = child.WaitForExitAsync(ct);
                if (await Task.WhenAny(health, exit) == exit) { timeout.Cancel(); try { await health; } catch (OperationCanceledException) { } throw new IOException("程序在启动确认前退出。"); }
                await health;
                if (child.HasExited) throw new IOException("程序在启动确认时退出。");
                if (launch.Trial) await store.ConfirmHealthyAsync(launch.Id, ct);
                using (var writer = new StreamWriter(pipe, leaveOpen: true) { AutoFlush = true }) await writer.WriteLineAsync("accepted");
                await exit;
            }
            catch (Exception error)
            {
                // Read the exit code before killing anything: a child that exited on its own carries the reason
                // (0xC000027B is a stowed WinRT/XAML exception, 0xC0000135 a missing DLL, 0xE0434352 a managed
                // exception), while one this launcher has to kill must never be reported as if it had chosen to
                // exit. Until 2026-10-12 neither was recorded anywhere, and "the program will not open at all"
                // on one player's machine needed an ETW session on that machine to explain.
                bool exited = false; int exitCode = 0;
                try { exited = child.HasExited; if (exited) exitCode = child.ExitCode; } catch (InvalidOperationException) { }
                try { if (child.Id > 0 && !child.HasExited) { child.Kill(true); await child.WaitForExitAsync(CancellationToken.None); } } catch (InvalidOperationException) { }
                LastFailure = exited
                    ? $"子进程退出码：0x{exitCode:X8}{DescribeExit(exitCode)}"
                    : $"等待程序启动确认超过 {healthTimeout.TotalMinutes:0.#} 分钟，子进程已被终止。";
                var diagnostic = errorOutput is null ? "" : await errorOutput;
                try { await File.AppendAllTextAsync(Path.Combine(store.Root, "launcher.log"), $"{DateTimeOffset.UtcNow:O} {launch.Id} {(exited ? $"exit=0x{exitCode:X8}" : "exit=killed")} {error}\n{diagnostic}\n", CancellationToken.None); }
                catch (IOException) { } catch (UnauthorizedAccessException) { }
                if (launch.Trial && retryFallback && !ct.IsCancellationRequested) { retryFallback = false; continue; }
                throw;
            }
            if (!store.ReadState().Restart) return;
        }
    }

    private static async Task ReadHealthAsync(NamedPipeServerStream pipe, Process child, string nonce, string version, CancellationToken ct)
    {
        await pipe.WaitForConnectionAsync(ct);
        if (OperatingSystem.IsWindows() && (!GetNamedPipeClientProcessId(pipe.SafePipeHandle, out uint pid) || pid != child.Id)) throw new IOException("启动确认来自错误的进程。");
        using var reader = new StreamReader(pipe, leaveOpen: true);
        var line = await reader.ReadLineAsync(ct);
        if (line is null || line.Length > 8192) throw new InvalidDataException("程序启动确认无效。");
        var health = JsonSerializer.Deserialize<ProgramHealth>(line);
        if (health is null || health.Nonce != nonce || health.ProcessId != child.Id || health.Version != version || string.IsNullOrWhiteSpace(health.ResourceSnapshotId))
            throw new InvalidDataException("程序版本或资源启动确认不匹配。");
    }

    /// <summary>
    /// Names the exit codes this program has actually died with on players' machines.
    /// </summary>
    /// <remarks>
    /// Only codes a support conversation has already needed are listed, and anything else stays a bare number:
    /// a guess in the dialog would be worse than the hex.
    /// </remarks>
    private static string DescribeExit(int code) => code switch
    {
        unchecked((int)0xC000027B) => "（启动期的 WinRT/XAML 异常，窗口未能创建）",
        unchecked((int)0xC0000135) => "（缺少 DLL）",
        unchecked((int)0xC0000142) => "（DLL 初始化失败）",
        unchecked((int)0xC0000005) => "（访问冲突：常见于注入、驱动或安全软件）",
        unchecked((int)0xE0434352) => "（.NET 未处理异常）",
        _ => "",
    };

    /// <summary>
    /// Keeps the last few kilobytes of a child's standard error.
    /// </summary>
    /// <remarks>
    /// Reading to the end would accumulate for the child's whole lifetime, and this launcher lives exactly as
    /// long as the child does, so only the tail is kept. Nothing here may throw into the caller: this is a
    /// diagnostic, and losing it must never mask the launch failure it was collected for.
    /// </remarks>
    private static async Task<string> DrainTailAsync(StreamReader reader)
    {
        var tail = new StringBuilder();
        var buffer = new char[4096];
        try
        {
            int count;
            while ((count = await reader.ReadAsync(buffer, 0, buffer.Length).ConfigureAwait(false)) > 0)
            {
                tail.Append(buffer, 0, count);
                if (tail.Length > 8192) tail.Remove(0, tail.Length - 8192);
            }
        }
        catch (Exception error) when (error is IOException or ObjectDisposedException or InvalidOperationException or OperationCanceledException) { }
        return tail.ToString();
    }

    public static FileStream AcquireChildLease(string installRoot)
    {
        if (!int.TryParse(Environment.GetEnvironmentVariable("IMAO_LAUNCH_PARENT"), out int pid) || !long.TryParse(Environment.GetEnvironmentVariable("IMAO_LAUNCH_PARENT_START"), out long ticks) || !UpdateStorage.IsProcessAlive(pid, ticks))
            throw new IOException("启动器已退出，请重新打开软件。");
        ProgramProcessJob.JoinFromChild();
        var path = Path.Combine(installRoot, "ProgramUpdates", "running.lock"); UpdateStorage.RejectLink(path);
        return new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetNamedPipeClientProcessId(SafePipeHandle pipe, out uint clientProcessId);
}
