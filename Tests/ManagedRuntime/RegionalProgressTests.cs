using IMao_WinUI.Helpers;

internal static class RegionalProgressTests
{
    public static void Run(string root, Action<bool, string> check)
    {
        string path = Path.Combine(root, "regional-progress.json");
        var reader = new RegionalProgressReader();
        void Write(string key) => File.WriteAllText(path, "{\"points\":{\"" + key + "\":{\"completed\":true},\"8:no\":{\"completed\":false}}}");
        Write("8:a");
        var first = reader.ReadAsync(path).GetAwaiter().GetResult();
        check(first.CompletedKeys.SetEquals(["8:a"]), "regional progress reads completed points only");
        // An unchanged file must not be opened again: deny file contents while metadata remains available.
        using (var locked = new FileStream(path, FileMode.Open, FileAccess.ReadWrite, FileShare.None))
        {
            var cached = reader.ReadAsync(path).GetAwaiter().GetResult();
            check(ReferenceEquals(first, cached), "unchanged progress reuses cached snapshot without reopening the file");
        }
        var stamp = File.GetLastWriteTimeUtc(path);
        Write("3:b");
        File.SetLastWriteTimeUtc(path, stamp.AddSeconds(2));
        var changed = reader.ReadAsync(path).GetAwaiter().GetResult();
        check(changed.CompletedKeys.Count == first.CompletedKeys.Count && changed.CompletedKeys.SetEquals(["3:b"])
            && !ReferenceEquals(first, changed), "same-count changes move completion to the correct region");
        // Native completion events bypass metadata caching, including restored timestamps.
        stamp = File.GetLastWriteTimeUtc(path);
        Write("8:c");
        File.SetLastWriteTimeUtc(path, stamp);
        var forced = reader.ReadAsync(path, force: true).GetAwaiter().GetResult();
        check(forced.CompletedKeys.SetEquals(["8:c"]), "explicit completion change refreshes identical file metadata");
        var missing = reader.ReadAsync(path + ".missing").GetAwaiter().GetResult();
        check(missing.CompletedKeys.Count == 0, "switching to a new ledger does not reuse previous progress");
        File.WriteAllText(path, "{");
        bool failed = false;
        try { reader.ReadAsync(path, true).GetAwaiter().GetResult(); }
        catch (System.Text.Json.JsonException) { failed = true; }
        check(failed, "invalid progress is reported rather than silently treated as zero completion");
        foreach (string invalidSchema in new[] { "[]", "{}", "{\"points\":[]}" })
        {
            File.WriteAllText(path, invalidSchema);
            failed = false;
            try { reader.ReadAsync(path, true).GetAwaiter().GetResult(); }
            catch (System.Text.Json.JsonException) { failed = true; }
            check(failed, "invalid progress schema is reported as a recoverable read failure: " + invalidSchema);
        }
        Write("8:d");
        check(reader.ReadAsync(path).GetAwaiter().GetResult().CompletedKeys.SetEquals(["8:d"]), "progress recovers after a failed read");
        File.Delete(path);
        check(reader.ReadAsync(path).GetAwaiter().GetResult().CompletedKeys.Count == 0, "deleted ledger clears cached completion");
    }
}
