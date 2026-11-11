using System.Diagnostics;
using System.Security.Cryptography;
using System.Text.Json;
using IMao_WinUI.Core.Updates;

static class VerificationTests
{
    internal static async Task Benchmark(string envelope)
    {
        using var json = JsonDocument.Parse(await File.ReadAllBytesAsync(envelope));
        var catalog = JsonSerializer.Deserialize<UpdateCatalog>(Convert.FromBase64String(json.RootElement.GetProperty("payload").GetString()!), UpdateJson.Options)!;
        var directory = Path.Combine(Path.GetDirectoryName(envelope)!, "app");
        Console.WriteLine($"files={catalog.App.Package!.Files.Count} bytes={catalog.App.Package.Files.Sum(f => f.Size)}; warm-cache, no installed files written");
        for (int round = 0; round < 4; round++)
            foreach (int degree in (round % 2 == 0 ? new[] { 1, 2, 4, 8 } : new[] { 8, 4, 2, 1 }))
            {
                var watch = Stopwatch.StartNew();
                await ProgramPackageValidation.VerifyDirectoryAsync(directory, catalog.App, degree);
                Console.WriteLine($"round={round} concurrency={degree} elapsed_ms={watch.ElapsedMilliseconds}");
            }
    }

    internal static async Task Run(string output)
    {
        var directory = Path.Combine(output, "verification"); Directory.CreateDirectory(directory);
        var build = new BuildInfo { AppVersion = "2026.11.1.1", BaselineId = "baseline", SourceCommit = new string('a', 40) };
        var files = new List<ResourceFile>();
        foreach (var path in ProgramPackageValidation.RequiredFiles.Concat(Enumerable.Range(0, 64).Select(i => $"data/{i}.bin")))
        {
            var bytes = path == "build-info.json" ? JsonSerializer.SerializeToUtf8Bytes(build, UpdateJson.Options) : new byte[131071 + files.Count];
            var target = Path.Combine(directory, path); Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            await File.WriteAllBytesAsync(target, bytes);
            files.Add(new ResourceFile { Path = path, Size = bytes.Length, Sha256 = Convert.ToHexString(SHA256.HashData(bytes)) });
        }
        var release = new ProgramRelease { Version = build.AppVersion, Package = new ProgramPackage { BaselineId = build.BaselineId, SourceCommit = build.SourceCommit,
            Url = "https://github.com/kahvia-d/WWMAP-TOOLS/releases/download/v2026.11.1.1/program.zip", Size = 1, Sha256 = new string('a', 64), Files = files } };
        foreach (int concurrency in new[] { 1, 2, 4, 8 })
        {
            var progress = new SerializedProgress();
            await ProgramPackageValidation.VerifyDirectoryAsync(directory, release, concurrency, progress: progress);
            if (progress.Last != files.Sum(f => f.Size) || progress.Count != files.Count + 1) throw new Exception("Incomplete progress");
            var target = Path.Combine(directory, files[^1].Path);
            using (var stream = File.OpenWrite(target)) stream.WriteByte(1);
            await Reject(() => ProgramPackageValidation.VerifyDirectoryAsync(directory, release, concurrency));
            using (var stream = File.OpenWrite(target)) stream.WriteByte(0);
            File.Move(target, target + ".extra");
            await Reject(() => ProgramPackageValidation.VerifyDirectoryAsync(directory, release, concurrency));
            File.Delete(target + ".extra");
            await Reject(() => ProgramPackageValidation.VerifyDirectoryAsync(directory, release, concurrency));
            await File.WriteAllBytesAsync(target, new byte[checked((int)files[^1].Size)]);
            using var cancel = new CancellationTokenSource();
            try
            {
                await ProgramPackageValidation.VerifyDirectoryAsync(directory, release, concurrency, cancel.Token, new CancelProgress(cancel));
                throw new Exception("Cancellation was ignored");
            }
            catch (OperationCanceledException) when (cancel.IsCancellationRequested) { }
        }
        Console.WriteLine("PASS full verification at 1/2/4/8: corrupt/extra/missing/cancellation and serialized monotonic progress");
    }
    static async Task Reject(Func<Task> action)
    {
        try { await action(); } catch (InvalidDataException) { return; }
        throw new Exception("Invalid directory accepted");
    }
    sealed class CancelProgress(CancellationTokenSource cancel) : IProgress<UpdateProgress>
    {
        public void Report(UpdateProgress value) { if (value.Completed > 0) cancel.Cancel(); }
    }
    sealed class SerializedProgress : IProgress<UpdateProgress>
    {
        int active;
        public long Last;
        public int Count;
        public void Report(UpdateProgress value)
        {
            if (Interlocked.Increment(ref active) != 1) throw new Exception("Concurrent progress callback");
            try
            {
                Thread.SpinWait(10000);
                if (value.Completed < Last || value.Completed > value.Total) throw new Exception("Nonmonotonic progress");
                Last = value.Completed; Count++;
            }
            finally { Interlocked.Decrement(ref active); }
        }
    }
}
