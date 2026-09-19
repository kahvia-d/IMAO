#nullable enable
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text.Json;

namespace IMao_WinUI.Core.Updates;

internal static class UpdateStorage
{
    internal static async Task<FileStream> LockAsync(string root, CancellationToken ct)
    {
        Directory.CreateDirectory(root);
        var path = Path.Combine(root, ".update.lock");
        while (true)
        {
            ct.ThrowIfCancellationRequested();
            try { return new FileStream(path, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None); }
            catch (IOException ex) when ((ex.HResult & 0xFFFF) is 32 or 33) { await Task.Delay(75, ct).ConfigureAwait(false); }
        }
    }

    internal static T Read<T>(string path) where T : new() => File.Exists(path)
        ? JsonSerializer.Deserialize<T>(File.ReadAllBytes(path), UpdateJson.Options) ?? throw new InvalidDataException("更新状态文件为空。")
        : new T();

    internal static async Task WriteAsync<T>(string path, T value, CancellationToken ct)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var temp = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            await using (var file = new FileStream(temp, FileMode.CreateNew, FileAccess.Write, FileShare.None, 65536, FileOptions.WriteThrough))
            {
                await JsonSerializer.SerializeAsync(file, value, UpdateJson.Options, ct).ConfigureAwait(false);
                await file.FlushAsync(ct).ConfigureAwait(false);
                file.Flush(true);
            }
            ct.ThrowIfCancellationRequested();
            await RenameAsync(() => File.Move(temp, path, true), ct).ConfigureAwait(false);
        }
        finally
        {
            // A scanner may briefly retain the scratch file. Cleanup must not obscure a failed commit.
            try { if (File.Exists(temp)) File.Delete(temp); }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }
    }

    internal static Task MoveDirectoryAsync(string source, string destination, CancellationToken ct) =>
        RenameAsync(() => Directory.Move(source, destination), ct);

    private static async Task RenameAsync(Action rename, CancellationToken ct)
    {
        // Windows scanners/readers can momentarily deny delete-sharing after all our own streams close.
        // Retry the same atomic operation only; never remove the destination or weaken validation.
        for (var attempt = 0; ; attempt++)
        {
            ct.ThrowIfCancellationRequested();
            try { rename(); return; }
            catch (Exception ex) when (attempt < 6 && (ex is IOException or UnauthorizedAccessException) && (ex.HResult & 0xFFFF) is 5 or 32 or 33)
            {
                await Task.Delay(Math.Min(50 << attempt, 250), ct).ConfigureAwait(false);
            }
        }
    }

    internal static string SafeChild(string root, string relative)
    {
        ValidateRelativePath(relative);
        var fullRoot = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar) + Path.DirectorySeparatorChar;
        var full = Path.GetFullPath(Path.Combine(fullRoot, relative.Replace('/', Path.DirectorySeparatorChar)));
        if (!full.StartsWith(fullRoot, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("资源路径超出目标目录。");
        return full;
    }

    internal static void ValidateRelativePath(string path)
    {
        if (string.IsNullOrWhiteSpace(path) || path.Length > 240 || path.Contains('\\') || path.StartsWith('/') || path.Contains(':') || path.Contains('\0'))
            throw new InvalidDataException("资源路径无效。");
        foreach (var part in path.Split('/'))
        {
            if (part.Length == 0 || part is "." or ".." || part.EndsWith(' ') || part.EndsWith('.') || part.IndexOfAny(['<', '>', '"', '|', '?', '*']) >= 0 || part.Any(char.IsControl))
                throw new InvalidDataException("资源路径包含不安全的名称。");
            var stem = part.Split('.')[0];
            if (stem.Equals("CON", StringComparison.OrdinalIgnoreCase) || stem.Equals("PRN", StringComparison.OrdinalIgnoreCase) || stem.Equals("AUX", StringComparison.OrdinalIgnoreCase) || stem.Equals("NUL", StringComparison.OrdinalIgnoreCase) ||
                (stem.Length == 4 && (stem.StartsWith("COM", StringComparison.OrdinalIgnoreCase) || stem.StartsWith("LPT", StringComparison.OrdinalIgnoreCase)) && char.IsDigit(stem[3])))
                throw new InvalidDataException("资源路径包含系统保留名称。");
        }
    }

    internal static void ValidateId(string value)
    {
        if (string.IsNullOrEmpty(value) || value.Length > 100 || !char.IsAsciiLetterOrDigit(value[0]) || value.Any(c => !char.IsAsciiLetterOrDigit(c) && c is not '-' and not '_' and not '.'))
            throw new InvalidDataException("资源标识或版本格式无效。");
    }

    internal static void RejectLink(string path)
    {
        var node = Path.GetFullPath(path);
        while (!string.IsNullOrEmpty(node))
        {
            if ((File.Exists(node) || Directory.Exists(node)) && (File.GetAttributes(node) & FileAttributes.ReparsePoint) != 0)
                throw new InvalidDataException("资源目录不能包含符号链接或目录联接：" + node);
            var parent = Path.GetDirectoryName(node);
            if (parent == node) break;
            node = parent;
        }
    }

    internal static async Task VerifyFileAsync(string path, ResourceFile expected, CancellationToken ct)
    {
        RejectLink(path);
        if (!File.Exists(path) || new FileInfo(path).Length != expected.Size) throw new InvalidDataException($"资源文件长度不符：{expected.Path}");
        await using var input = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 131072, FileOptions.Asynchronous | FileOptions.SequentialScan);
        var actual = Convert.ToHexString(await SHA256.HashDataAsync(input, ct).ConfigureAwait(false));
        if (!actual.Equals(expected.Sha256, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException($"资源文件校验失败：{expected.Path}");
    }

    internal static async Task VerifyDirectoryAsync(string directory, IReadOnlyList<ResourceFile> files, CancellationToken ct)
    {
        RejectLink(directory);
        var expected = new HashSet<string>(files.Select(f => f.Path), StringComparer.OrdinalIgnoreCase);
        if (expected.Count != files.Count || expected.Count == 0) throw new InvalidDataException("资源包文件清单为空或重复。");
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var remaining = new Stack<string>();
        remaining.Push(directory);
        while (remaining.Count > 0)
        {
            ct.ThrowIfCancellationRequested();
            foreach (var path in Directory.EnumerateFileSystemEntries(remaining.Pop()))
            {
                RejectLink(path);
                if (Directory.Exists(path)) { remaining.Push(path); continue; }
                var relative = Path.GetRelativePath(directory, path).Replace('\\', '/');
                if (!expected.Contains(relative) || !seen.Add(relative)) throw new InvalidDataException("资源目录包含清单之外的文件。");
            }
        }
        if (seen.Count != expected.Count) throw new InvalidDataException("资源目录缺少清单文件。");
        foreach (var file in files)
        {
            UpdateSignature.ValidateResourceFile(file);
            await VerifyFileAsync(SafeChild(directory, file.Path), file, ct).ConfigureAwait(false);
        }
    }

    internal static bool IsProcessAlive(int pid, long startUtcTicks)
    {
        try
        {
            using var process = Process.GetProcessById(pid);
            return !process.HasExited && process.StartTime.ToUniversalTime().Ticks == startUtcTicks;
        }
        catch (ArgumentException) { return false; }
        catch (InvalidOperationException) { return false; }
        // If another process cannot be inspected, do not consume its activation attempt.
        catch (System.ComponentModel.Win32Exception) { return true; }
    }
}
