#nullable enable
using System.Diagnostics;
using System.Runtime.InteropServices;
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

    /// <summary>
    /// Refuses a path that is, or sits under, a symbolic link or a directory junction, up to the root of the
    /// volume. Every caller that owns a directory should use <see cref="RejectLink(string, string)"/> instead.
    /// </summary>
    /// <remarks>
    /// The test is <see cref="FileAttributes.ReparsePoint"/> and nothing else, and that is deliberate rather
    /// than incidental: a hard link is not a reparse point. It is a second directory entry for the same file,
    /// which the update system uses on purpose - a runtime library this release does not change is shared with
    /// the version already running instead of being copied, and the staged tree is still complete and is still
    /// hashed in full before it is published. <b>Do not tighten this into a link count.</b> An installation that
    /// has already updated holds shared files by design, so that check would make every one of them fail its own
    /// startup verification, and no update could repair a client that cannot start.
    /// </remarks>
    internal static void RejectLink(string path) => RejectLink(path, null);

    /// <summary>
    /// Refuses a path that is, or sits under, a symbolic link or a directory junction - counting from
    /// <paramref name="ownedRoot"/> downwards, and stopping there.
    /// </summary>
    /// <param name="ownedRoot">
    /// The directory the calling operation owns: the installation root for the program update store, the update
    /// root for the resource store. It is the boundary of this check, and it is checked too, so a link at the
    /// root itself is still refused.
    /// </param>
    /// <remarks>
    /// <para>
    /// Why the walk stops at an owned root, and why that is not a weakening. <b>This program can only vouch for
    /// a tree it placed itself.</b> Above that tree sit the installation's parent directories, the player's
    /// profile, the Steam library and - on SteamOS, where this program runs under Proton - the compatibility
    /// layer's own path mapping. Wine surfaces a reparse point for the *named* component that is a symlink, and
    /// a Steam Deck has several by design: <c>~/.steam/steam</c> points into <c>~/.local/share/Steam</c>, and an
    /// immutable-root distribution such as Bazzite keeps <c>/home</c> as a symlink to <c>/var/home</c>. Walking
    /// to the volume root therefore reported the first of those and refused a perfectly ordinary installation:
    /// the 2026-10 Steam Deck report was <c>Z:\home</c>, a component above the program, on a path whose own
    /// directories were all real. Nothing above an owned root can redirect a path *inside* it either: resolution
    /// of those components is a prefix, shared identically by the owned root and by everything below it, and
    /// every component between the owned root and the path is still checked here. Containment is therefore
    /// preserved after resolution, which is what <see cref="SafeChild"/> relies on.
    /// </para>
    /// <para>
    /// What is *not* given up: an attacker who can put a link inside the owned tree - the case that actually
    /// matters, because that is where extraction, version switching, rollback and recursive deletion run - is
    /// refused exactly as before. What is given up is refusing an installation merely because the player or the
    /// platform put the installation itself under a link, which grants an attacker nothing they did not already
    /// have: the program was started from that path, so whoever could repoint it could replace its executable.
    /// </para>
    /// <para>
    /// No operating system is detected anywhere in this method and no path is ever allow-listed by name. A
    /// boundary is a property of the operation, not of the environment, so this behaves identically on Windows
    /// and under Proton; the difference is only which directories a caller owns.
    /// </para>
    /// </remarks>
    internal static void RejectLink(string path, string? ownedRoot)
    {
        var node = Path.GetFullPath(path);
        string? boundary = null;
        if (ownedRoot is not null)
        {
            boundary = NormalizeRoot(ownedRoot);
            if (!IsAtOrBelow(node, boundary)) throw new InvalidDataException("资源目录超出本程序管理的范围：" + node);
        }
        var reachedBoundary = false;
        // Deepest first, and nothing is collected: this runs once per file of every verification, and a real
        // installation verifies tens of thousands of them.
        for (var cursor = node; ; )
        {
            if (IsLink(cursor))
            {
                UpdatePathDiagnostics.Record(UpdatePathDiagnostics.Refusal, cursor, DescribeLink(cursor, boundary));
                throw new InvalidDataException(boundary is null
                    ? "资源目录不能包含符号链接或目录联接：" + cursor
                    : "资源目录不能包含符号链接或目录联接：" + cursor + "（该链接在本程序受管理的目录之内：" + boundary + "。请把安装目录与更新目录放在没有链接的位置，或先移除这个链接。）");
            }
            if (boundary is not null && cursor.Equals(boundary, StringComparison.OrdinalIgnoreCase)) { reachedBoundary = true; break; }
            var parent = Path.GetDirectoryName(cursor);
            if (string.IsNullOrEmpty(parent) || parent == cursor) break;
            cursor = parent;
        }
        // Only ancestors strictly above the boundary are left, and only when the boundary really is where the
        // walk stopped - so a boundary that was never reached never reports an environment it does not have.
        if (reachedBoundary) ReportAncestorsAbove(boundary!);
    }

    /// <summary>True when the directory entry itself is a symbolic link or a junction.</summary>
    private static bool IsLink(string node)
    {
        // Existence first, and on purpose: a path that is not there yet is the normal case for everything this
        // program is about to create, and File.Exists/Directory.Exists report a dangling link as absent, which
        // is how this behaved before there was a boundary.
        if (!File.Exists(node) && !Directory.Exists(node)) return false;
        try { return (File.GetAttributes(node) & FileAttributes.ReparsePoint) != 0; }
        catch (Exception error) when (error is UnauthorizedAccessException or IOException)
        {
            // Not a security decision: the path could not be inspected at all. Reported, then left to the
            // caller exactly as it was before, so no check downstream starts trusting a path this could not read.
            UpdatePathDiagnostics.Record(UpdatePathDiagnostics.Inaccessible, node, error.GetType().Name);
            throw;
        }
    }

    /// <summary>Names the link for the log, and resolves it when the platform will say where it points.</summary>
    private static string DescribeLink(string node, string? boundary)
    {
        string location = boundary is null ? "受管理目录未声明" : "位于受管理目录之内：" + boundary;
        try
        {
            var target = Directory.Exists(node) ? new DirectoryInfo(node).ResolveLinkTarget(true)?.FullName
                : new FileInfo(node).ResolveLinkTarget(true)?.FullName;
            return target is null ? location : location + " 指向：" + target;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or NotSupportedException) { return location; }
    }

    private static readonly object AncestorGate = new();
    private static readonly HashSet<string> InspectedBoundaries = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>
    /// Records, once per owned root, that an ancestor above it is a link.
    /// </summary>
    /// <remarks>
    /// Informational only and never a decision: this is what makes "the compatibility layer maps the path" a
    /// line in the log instead of an exception. Once per root, because this runs on every file of every
    /// verification and a real installation verifies tens of thousands of them.
    /// </remarks>
    private static void ReportAncestorsAbove(string boundary)
    {
        lock (AncestorGate) { if (!InspectedBoundaries.Add(boundary)) return; }
        try
        {
            for (var node = Path.GetDirectoryName(boundary); !string.IsNullOrEmpty(node); )
            {
                if (IsLink(node))
                {
                    UpdatePathDiagnostics.Record(UpdatePathDiagnostics.Mapping, node,
                        "位于受管理目录之上：" + boundary + "；" + DescribeLink(node, null));
                    return;
                }
                var parent = Path.GetDirectoryName(node);
                if (parent == node) return;
                node = parent;
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException) { }
    }

    /// <summary>A full path without a trailing separator, keeping the separator a volume root needs.</summary>
    private static string NormalizeRoot(string root)
    {
        var full = Path.GetFullPath(root);
        var trimmed = full.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        // "C:" is a drive-relative path, not the root of a drive: put the separator back when trimming ate it.
        return trimmed.Length == 2 && trimmed[1] == ':' ? trimmed + Path.DirectorySeparatorChar : trimmed;
    }

    /// <summary>True when <paramref name="path"/> is <paramref name="root"/> or sits under it. Both are normalized.</summary>
    private static bool IsAtOrBelow(string path, string root) =>
        path.Equals(root, StringComparison.OrdinalIgnoreCase) ||
        path.StartsWith(root.EndsWith(Path.DirectorySeparatorChar) ? root : root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase);

    /// <summary>
    /// Gives <paramref name="linkPath"/> the same bytes as <paramref name="existingPath"/>, or reports that this
    /// filesystem will not do it.
    /// </summary>
    /// <remarks>
    /// Always an optimisation, never a requirement: the caller copies when this returns false, so a different
    /// volume, a network share, a filesystem without hard links or a policy that forbids them all end in the
    /// same complete and verified tree - only a larger one.
    /// </remarks>
    internal static bool TryHardLink(string linkPath, string existingPath)
    {
        try { return CreateHardLinkW(linkPath, existingPath, IntPtr.Zero); }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or PlatformNotSupportedException or DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CreateHardLinkW(string linkFileName, string existingFileName, IntPtr securityAttributes);

    internal static async Task VerifyFileAsync(string path, ResourceFile expected, CancellationToken ct, string? ownedRoot = null)
    {
        RejectLink(path, ownedRoot);
        if (!File.Exists(path) || new FileInfo(path).Length != expected.Size) throw new InvalidDataException($"资源文件长度不符：{expected.Path}");
        await using var input = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 131072, FileOptions.Asynchronous | FileOptions.SequentialScan);
        var actual = Convert.ToHexString(await SHA256.HashDataAsync(input, ct).ConfigureAwait(false));
        if (!actual.Equals(expected.Sha256, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException($"资源文件校验失败：{expected.Path}");
    }

    internal static async Task VerifyDirectoryAsync(string directory, IReadOnlyList<ResourceFile> files, CancellationToken ct, string? ownedRoot = null)
    {
        RejectLink(directory, ownedRoot);
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
                RejectLink(path, ownedRoot);
                if (Directory.Exists(path)) { remaining.Push(path); continue; }
                var relative = Path.GetRelativePath(directory, path).Replace('\\', '/');
                if (!expected.Contains(relative) || !seen.Add(relative)) throw new InvalidDataException("资源目录包含清单之外的文件。");
            }
        }
        if (seen.Count != expected.Count) throw new InvalidDataException("资源目录缺少清单文件。");
        foreach (var file in files)
        {
            UpdateSignature.ValidateResourceFile(file);
            await VerifyFileAsync(SafeChild(directory, file.Path), file, ct, ownedRoot).ConfigureAwait(false);
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
