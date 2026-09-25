#nullable enable
using System.IO.Compression;

namespace IMao_WinUI.Core.Updates;

/// <summary>
/// Provides program files from the package MirrorChyan serves for the release being installed.
///
/// The archive is never trusted and never has to be. It is treated as a bag of candidate files: only paths
/// the signed catalog declares are taken out of it, each one is held to the size and SHA-256 that catalog
/// recorded, and everything else the archive contains is ignored - including the control file an
/// incremental package carries to describe itself. Whatever it does not provide is downloaded from the
/// signed shards as usual, and the assembled directory still has to satisfy the catalog exactly.
///
/// That is why nothing here verifies the archive itself. MirrorChyan's whole package is byte-for-byte our
/// published archive, but the incremental one is assembled by MirrorChyan from two of them, so its digest
/// matches nothing we ever signed. The per-file records are what make a foreign archive safe to read.
/// </summary>
public sealed class MirrorChyanProgramSource : IProgramFileSupplier
{
    private readonly MirrorChyanPackage _package;
    private readonly Func<Uri, CancellationToken, Task<HttpResponseMessage>> _fetch;
    private readonly string _scratchDirectory;
    private readonly IProgress<UpdateProgress>? _progress;

    /// <param name="fetch">
    /// How to reach the URL. Supplied by the caller so the transport - the mirror host rules and the
    /// redirect that carries a time-limited key - stays in one place instead of being reimplemented here.
    /// </param>
    public MirrorChyanProgramSource(MirrorChyanPackage package,
        Func<Uri, CancellationToken, Task<HttpResponseMessage>> fetch, string scratchDirectory, IProgress<UpdateProgress>? progress = null)
    {
        _package = package;
        _fetch = fetch;
        _scratchDirectory = scratchDirectory;
        _progress = progress;
    }

    /// <summary>
    /// How many files the last <see cref="SupplyAsync"/> actually handed over, so the caller can describe the
    /// transport truthfully. Zero means the mirror contributed nothing and the signed shards did the work.
    /// </summary>
    public int SuppliedCount { get; private set; }

    public async Task<IReadOnlySet<string>> SupplyAsync(ProgramPackage package, string app, CancellationToken ct)
    {
        SuppliedCount = 0;
        var supplied = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var candidates = new List<string>();
        var archive = Path.Combine(_scratchDirectory, "mirrorchyan-" + Guid.NewGuid().ToString("N") + ".zip");
        try
        {
            if (!Uri.TryCreate(_package.Url, UriKind.Absolute, out var uri)) return supplied;
            var expected = package.Files.ToDictionary(f => f.Path, StringComparer.OrdinalIgnoreCase);
            // The archive's own length is not signed, so the ceiling that means anything is the tree it
            // could possibly produce: a zip cannot be meaningfully larger than what is inside it.
            var ceiling = package.Files.Sum(f => f.Size) + 16L * 1024 * 1024;
            _progress?.Report(new UpdateProgress("从 Mirror酱下载新版程序", 0, 0));
            Directory.CreateDirectory(_scratchDirectory);
            using (var response = await _fetch(uri, ct).ConfigureAwait(false))
            {
                await using var input = await response.Content.ReadAsStreamAsync(ct).ConfigureAwait(false);
                await using var output = new FileStream(archive, FileMode.CreateNew, FileAccess.Write, FileShare.None, 131072, FileOptions.Asynchronous);
                await CopyAsync(input, output, ceiling, ct).ConfigureAwait(false);
            }
            using var zip = ZipFile.OpenRead(archive);
            var entries = ProgramPackageValidation.ReadArchiveEntries(zip, expected, ProgramPackageValidation.ArchiveScope.Partial);
            // A package that said it was the whole archive and turns out not to be gets no benefit of the
            // doubt; the signed shards are the better answer.
            if (_package.IsWholePackage && entries.Count != expected.Count) return supplied;
            var provided = package.Files.Where(f => entries.ContainsKey(f.Path)).ToList();
            if (provided.Count == 0) return supplied;
            // Recorded before extracting, so a failure part-way through can undo everything this source may
            // have touched. The fallback writes into the same directory with CreateNew, which a leftover file
            // would break.
            candidates.AddRange(provided.Select(f => f.Path));
            _progress?.Report(new UpdateProgress("校验 Mirror酱提供的文件", 0, 0));
            await ProgramPackageValidation.ExtractEntriesAsync(app, provided, entries, ct).ConfigureAwait(false);
            foreach (var path in candidates) supplied.Add(path);
            SuppliedCount = supplied.Count;
            return supplied;
        }
        catch (Exception ex) when (ex is IOException or InvalidDataException or HttpRequestException or TimeoutException or UnauthorizedAccessException
            || (ex is OperationCanceledException && !ct.IsCancellationRequested))
        {
            RollBack(app, candidates);
            return new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        }
        finally { TryDelete(archive); }
    }

    private static async Task CopyAsync(Stream input, Stream output, long ceiling, CancellationToken ct)
    {
        var buffer = new byte[131072];
        long copied = 0;
        while (true)
        {
            var count = await input.ReadAsync(buffer, ct).ConfigureAwait(false);
            if (count == 0) break;
            copied = checked(copied + count);
            if (copied > ceiling) throw new InvalidDataException("Mirror酱 的包超过了它可能的大小上限。");
            await output.WriteAsync(buffer.AsMemory(0, count), ct).ConfigureAwait(false);
        }
        if (copied == 0) throw new InvalidDataException("Mirror酱 返回了空包。");
    }

    /// <summary>
    /// Removes whatever this source wrote, so the signed transport can take the same paths over. Only paths
    /// this source chose to write are touched, and a file that never landed is simply absent.
    /// </summary>
    private static void RollBack(string app, List<string> paths)
    {
        foreach (var path in paths)
        {
            try
            {
                var target = UpdateStorage.SafeChild(app, path);
                if (File.Exists(target)) File.Delete(target);
            }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }
    }

    private static void TryDelete(string path)
    {
        try { if (File.Exists(path)) File.Delete(path); }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }
}
