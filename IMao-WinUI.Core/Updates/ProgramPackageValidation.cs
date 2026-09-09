#nullable enable
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.Json;

namespace IMao_WinUI.Core.Updates;

// Executable payloads have their own signed policy. Map packages continue to reject executables.
public static class ProgramPackageValidation
{
    public const int LauncherProtocol = 1;
    public static readonly string[] RequiredFiles = ["IMao-WinUI.exe", "IMao-WinUI.dll", "IMao-CoreHost.exe", "IMao-Launcher.exe", "build-info.json", "Assets/Updates/bundled-snapshot.json", "Assets/Updates/trusted-keys.json"];

    public static void Validate(ProgramPackage package)
    {
        UpdateSignature.ValidateUrl(package.Url, true);
        UpdateStorage.ValidateId(package.BaselineId);
        if (package.LauncherProtocol < 1 || package.Architecture != "win-x64" || package.SourceCommit is null || package.SourceCommit.Length != 40 || !package.SourceCommit.All(char.IsAsciiHexDigit) ||
            package.Size <= 0 || package.Size >= 2L * 1024 * 1024 * 1024 || !UpdateSignature.IsHash(package.Sha256) || package.Files is null || package.Files.Count is < 1 or > 30000)
            throw new InvalidDataException("程序包元数据无效或不支持此平台。");
        var paths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        long total = 0;
        foreach (var file in package.Files)
        {
            if (file is null) throw new InvalidDataException("程序文件清单包含空记录。");
            UpdateStorage.ValidateRelativePath(file.Path);
            if (file.Size < 0 || file.Size > 4L * 1024 * 1024 * 1024 || !UpdateSignature.IsHash(file.Sha256) || !paths.Add(file.Path))
                throw new InvalidDataException("程序文件清单存在无效或重复记录。");
            if (file.Path.Split('/').Any(p => new[] { "ProgramUpdates", "ResourceUpdates", "SavedPoints", "SavedRoutes", "Logs", ".git" }.Contains(p, StringComparer.OrdinalIgnoreCase)))
                throw new InvalidDataException("程序包不能包含更新状态或用户数据。");
            total = checked(total + file.Size);
            if (total > 8L * 1024 * 1024 * 1024) throw new InvalidDataException("程序包展开大小超过限制。");
        }
        if (RequiredFiles.Any(p => !paths.Contains(p))) throw new InvalidDataException("程序包缺少必需文件。");
        foreach (var path in paths)
        {
            var parent = path;
            while (parent.Contains('/'))
            {
                parent = parent[..parent.LastIndexOf('/')];
                if (paths.Contains(parent)) throw new InvalidDataException("程序文件与目录路径冲突。");
            }
        }
    }

    public static async Task ExtractAsync(string archive, string destination, ProgramPackage package, CancellationToken ct = default)
    {
        Validate(package);
        await UpdateStorage.VerifyFileAsync(archive, new ResourceFile { Path = "program.zip", Size = package.Size, Sha256 = package.Sha256 }, ct);
        if (Directory.Exists(destination)) throw new IOException("程序候选目录已经存在。");
        UpdateStorage.RejectLink(destination);
        using var zip = ZipFile.OpenRead(archive);
        var expected = package.Files.ToDictionary(f => f.Path, StringComparer.OrdinalIgnoreCase);
        var entries = new Dictionary<string, ZipArchiveEntry>(StringComparer.OrdinalIgnoreCase);
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var entry in zip.Entries)
        {
            var directory = entry.FullName.EndsWith('/');
            var name = directory ? entry.FullName[..^1] : entry.FullName;
            UpdateStorage.ValidateRelativePath(name);
            int type = (entry.ExternalAttributes >> 16) & 0xF000;
            if (!names.Add(name) || (entry.ExternalAttributes & (int)FileAttributes.ReparsePoint) != 0 || type is not (0 or 0x8000 or 0x4000))
                throw new InvalidDataException("程序压缩包包含链接、重复路径或特殊文件。");
            if (directory)
            {
                if (entry.Length != 0 || type == 0x8000 || !expected.Keys.Any(p => p.StartsWith(name + "/", StringComparison.OrdinalIgnoreCase))) throw new InvalidDataException("程序压缩包目录无效。");
                continue;
            }
            if (type == 0x4000 || !expected.TryGetValue(name, out var file) || entry.Length != file.Size) throw new InvalidDataException("程序压缩包与签名文件清单不符。");
            entries.Add(name, entry);
        }
        if (entries.Count != expected.Count) throw new InvalidDataException("程序压缩包缺少文件。");
        Directory.CreateDirectory(destination);
        foreach (var file in package.Files)
        {
            ct.ThrowIfCancellationRequested();
            var target = UpdateStorage.SafeChild(destination, file.Path);
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            UpdateStorage.RejectLink(target);
            await using (var input = entries[file.Path].Open())
            await using (var output = new FileStream(target, FileMode.CreateNew, FileAccess.Write, FileShare.None, 131072, FileOptions.Asynchronous))
            {
                var buffer = new byte[131072];
                long written = 0;
                int count;
                while ((count = await input.ReadAsync(buffer, ct)) != 0)
                {
                    written = checked(written + count);
                    if (written > file.Size) throw new InvalidDataException("程序文件展开长度超出限制。");
                    await output.WriteAsync(buffer.AsMemory(0, count), ct);
                }
            }
            await UpdateStorage.VerifyFileAsync(target, file, ct);
        }
    }

    public static async Task VerifyDirectoryAsync(string directory, ProgramRelease release, CancellationToken ct = default)
    {
        var package = release.Package ?? throw new InvalidDataException("程序更新缺少签名包信息。");
        Validate(package);
        var paths = new HashSet<string>(package.Files.Select(f => f.Path), StringComparer.OrdinalIgnoreCase);
        var pending = new Stack<string>(); pending.Push(directory);
        int seen = 0;
        while (pending.Count > 0)
        {
            var current = pending.Pop(); UpdateStorage.RejectLink(current);
            foreach (var path in Directory.EnumerateFileSystemEntries(current))
            {
                UpdateStorage.RejectLink(path);
                if (Directory.Exists(path)) { pending.Push(path); continue; }
                if (!paths.Contains(Path.GetRelativePath(directory, path).Replace('\\', '/'))) throw new InvalidDataException("程序目录包含清单之外的文件。");
                seen++;
            }
        }
        if (seen != paths.Count) throw new InvalidDataException("程序目录缺少文件。");
        foreach (var file in package.Files) await UpdateStorage.VerifyFileAsync(UpdateStorage.SafeChild(directory, file.Path), file, ct);
        var build = JsonSerializer.Deserialize<BuildInfo>(await File.ReadAllBytesAsync(Path.Combine(directory, "build-info.json"), ct), UpdateJson.Options);
        if (build is null || build.AppVersion != release.Version || build.BaselineId != package.BaselineId || build.SourceCommit != package.SourceCommit)
            throw new InvalidDataException("程序构建信息与签名清单不一致。");
    }

    public static async Task<ProgramPackage> DescribeAsync(string archive, BuildInfo build, string url, CancellationToken ct = default)
    {
        using var zip = ZipFile.OpenRead(archive);
        var files = new List<ResourceFile>();
        foreach (var entry in zip.Entries)
        {
            if (entry.FullName.EndsWith('/')) continue;
            await using var stream = entry.Open();
            files.Add(new() { Path = entry.FullName, Size = entry.Length, Sha256 = Convert.ToHexString(await SHA256.HashDataAsync(stream, ct)).ToLowerInvariant() });
        }
        await using var input = File.OpenRead(archive);
        var package = new ProgramPackage { SourceCommit = build.SourceCommit, BaselineId = build.BaselineId, Url = url, Size = input.Length,
            Sha256 = Convert.ToHexString(await SHA256.HashDataAsync(input, ct)).ToLowerInvariant(), Files = files };
        Validate(package);
        return package;
    }
}
