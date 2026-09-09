#nullable enable
using System.Diagnostics;
using System.IO.Pipes;
using System.Runtime.InteropServices;
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
    public ProgramLauncher(ProgramUpdateStore store, Func<string, ProcessStartInfo>? startFactory = null, TimeSpan? healthTimeout = null)
    {
        this.store = store;
        this.startFactory = startFactory ?? (directory => new ProcessStartInfo(Path.Combine(directory, "IMao-WinUI.exe")) { WorkingDirectory = directory, UseShellExecute = false });
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
                if (start.RedirectStandardError) errorOutput = child.StandardError.ReadToEndAsync();
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
                try { if (child.Id > 0 && !child.HasExited) { child.Kill(true); await child.WaitForExitAsync(CancellationToken.None); } } catch (InvalidOperationException) { }
                var diagnostic = errorOutput is null ? "" : await errorOutput;
                try { await File.AppendAllTextAsync(Path.Combine(store.Root, "launcher.log"), $"{DateTimeOffset.UtcNow:O} {launch.Id} {error}\n{diagnostic}\n", CancellationToken.None); }
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
