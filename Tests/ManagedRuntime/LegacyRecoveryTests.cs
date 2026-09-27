using IMao_WinUI.Services;
using System.Text.Json;

internal static class LegacyRecoveryTests
{
    /// <summary>
    /// Point data written before the record-book list existed has to be reachable again, and the
    /// recovery must never modify or delete what it found. See Docs/LegacyDataRecovery_20260927.md.
    /// </summary>
    public static void Run(string root, Action<bool, string> check)
    {
        string directory = Path.Combine(root, "legacy-recovery-" + Guid.NewGuid().ToString("N"));

        // A pre-rewrite file beside an OLD program directory — the case the current version can never
        // reach on its own, because it only ever looks next to itself (and only into "local").
        string program = Path.Combine(directory, "IMao-v2026.9.9.6");
        string old = Path.Combine(directory, "IMao-v9.9.6-old");
        Directory.CreateDirectory(Path.Combine(program, "SavedPoints"));
        Directory.CreateDirectory(Path.Combine(old, "SavedPoints"));
        string legacyPath = Path.Combine(old, "SavedPoints", "account_1.json");
        File.WriteAllText(legacyPath,
            "{\"World\":{\"sx_lgn\":[{\"id\":\"111\"},{\"id\":\"222\"}],\"cx_01\":[{\"id\":\"333\"}]}," +
            "\"LowerVault\":{\"jy\":[{\"id\":\"444\"}]},\"NotAScene\":{\"x\":[{\"id\":\"555\"}]}}");
        byte[] legacyBytes = File.ReadAllBytes(legacyPath);

        string savedPoints = Path.Combine(directory, "SavedPoints");
        Directory.CreateDirectory(savedPoints);
        File.WriteAllText(Path.Combine(savedPoints, "account_1.json"), "{\"World\":{\"cx_01\":[{\"id\":\"333\"},{\"id\":\"\"}]}}");

        // The record-book list is seeded BEFORE the extra documents appear, which is exactly how a
        // document ends up on disk without ever being listed.
        var catalog = new LocalAccountCatalog(Path.Combine(savedPoints, "accounts.json"), Path.Combine(savedPoints, "profiles"));
        Directory.CreateDirectory(Path.Combine(savedPoints, "profiles"));
        File.WriteAllText(Path.Combine(savedPoints, "profiles", "kuro_777.json"),
            "{\"schemaVersion\":2,\"profileId\":\"kuro_777\",\"revision\":3,\"syncStates\":[],\"points\":{" +
            "\"8:a\":{\"sceneName\":\"World\",\"nameId\":\"cx_01\",\"stateId\":8,\"pointId\":\"a\",\"completed\":true," +
            "\"remoteCompleted\":null,\"pending\":false}}}");
        File.WriteAllText(Path.Combine(savedPoints, "profiles", "broken.json"), "{\"schemaVersion\":1,\"points\":{}}");
        File.WriteAllText(Path.Combine(savedPoints, "profiles", "not-json.json"), "{ this is not json");
        byte[] unlistedBytes = File.ReadAllBytes(Path.Combine(savedPoints, "profiles", "kuro_777.json"));

        // "local" was deleted for real, which is why its document sits in deleted/ and not in profiles/.
        check(catalog.TryCreate("备用", "", out _, out _) && catalog.TryDelete("local", out _),
            "the test setup starts from a list where 默认 is genuinely gone");
        string deletedFolder = Path.Combine(savedPoints, "deleted", "20260927-094436-local");
        Directory.CreateDirectory(deletedFolder);
        File.WriteAllText(Path.Combine(deletedFolder, "local.json"),
            "{\"schemaVersion\":2,\"profileId\":\"local\",\"revision\":9,\"syncStates\":[],\"points\":{" +
            "\"903:b\":{\"sceneName\":\"Avinoleum\",\"nameId\":\"jx\",\"stateId\":903,\"pointId\":\"b\",\"completed\":true," +
            "\"remoteCompleted\":null,\"pending\":false}}}");
        byte[] deletedBytes = File.ReadAllBytes(Path.Combine(deletedFolder, "local.json"));

        var recovery = new LegacyPointRecovery(savedPoints, program);
        var scan = recovery.Scan(catalog.Accounts.Select(account => account.Id).ToArray());
        var singles = scan.Where(source => source.Kind == "pre-rewrite-record").ToList();
        check(singles.Count == 2, "both the program's own and a neighbouring installation's pre-rewrite file are found");
        var oldSource = singles.Single(source => source.Path == legacyPath);
        check(oldSource.Points == 4 && oldSource.Recoverable && !oldSource.AlreadyRecovered,
            "a pre-rewrite file reports its points, ignores unknown scenes and refuses an empty point id");
        check(oldSource.Regions.Count == 2 && oldSource.Regions.Sum(region => region.Count) == 4,
            "a pre-rewrite file is summarised by the scene it was keyed by");
        check(scan.Count(source => source.Kind == "unlisted-document" && source.Recoverable) == 1 &&
            scan.Single(source => source.Kind == "unlisted-document" && source.Recoverable).LedgerId == "kuro_777",
            "only the readable progress document is offered as a recoverable unlisted one");
        check(scan.Count(source => !source.Recoverable && source.Problem.Length > 0) == 2,
            "a document the store would reject, and one that is not JSON at all, are both reported with a reason");
        check(scan.Single(source => source.Kind == "deleted-document").LedgerId == "local" &&
            scan.Single(source => source.Kind == "deleted-document").Points == 1,
            "a document 删除 moved aside is found under its own id");

        var report = recovery.Recover(catalog);
        check(report.Recovered.Count == 3 && report.Notes.Count == 0,
            "a recovery brings back all three kinds and has nothing to complain about");

        // 1. The unlisted document is adopted exactly where it is: same bytes, same path.
        var adopted = catalog.Accounts.SingleOrDefault(account => account.Id == "kuro_777");
        check(adopted is not null && adopted.KuroAccountId == "777" &&
            File.ReadAllBytes(Path.Combine(savedPoints, "profiles", "kuro_777.json")).SequenceEqual(unlistedBytes),
            "an unlisted document becomes a record book in place, its binding read from its id, its bytes untouched");

        // 2. The deleted document is copied back under its own id, and the copy in deleted/ stays.
        check(catalog.Accounts.Any(account => account.Id == "local") &&
            File.ReadAllBytes(catalog.ProgressPath("local")).SequenceEqual(deletedBytes) &&
            File.ReadAllBytes(Path.Combine(deletedFolder, "local.json")).SequenceEqual(deletedBytes),
            "a record book 删除 moved aside is copied back under its own id and the deleted copy is left alone");

        // 3. The pre-rewrite data becomes one new record book the native store will accept.
        var combined = report.Recovered.Single(outcome => outcome.Action.Contains("新建"));
        check(combined.Points == 4 && catalog.Accounts.Any(account => account.Id == combined.LedgerId && !account.IsBound),
            "the pre-rewrite points become one new, unbound record book");
        using (var document = JsonDocument.Parse(File.ReadAllBytes(catalog.ProgressPath(combined.LedgerId))))
        {
            var body = document.RootElement;
            check(body.GetProperty("schemaVersion").GetInt32() == 2 &&
                body.GetProperty("profileId").GetString() == combined.LedgerId &&
                body.GetProperty("points").ValueKind == JsonValueKind.Object &&
                body.GetProperty("syncStates").ValueKind == JsonValueKind.Array,
                "the recovered document satisfies every condition the native store checks before it loads one");
            var points = body.GetProperty("points");
            bool shaped = true;
            int count = 0;
            foreach (var entry in points.EnumerateObject())
            {
                var record = entry.Value;
                ++count;
                shaped &= entry.Name == $"{record.GetProperty("stateId").GetInt32()}:{record.GetProperty("pointId").GetString()}" &&
                    record.GetProperty("sceneName").GetString()!.Length > 0 &&
                    record.GetProperty("nameId").GetString()!.Length > 0 &&
                    record.GetProperty("completed").GetBoolean() &&
                    record.GetProperty("pending").GetBoolean() &&
                    record.GetProperty("localTouched").GetBoolean() &&
                    record.GetProperty("remoteCompleted").ValueKind == JsonValueKind.Null;
            }
            check(shaped && count == 4,
                "every recovered point carries the identity and the fields the store's reader touches, queued for upload");
            check(points.TryGetProperty("8:111", out _) && points.TryGetProperty("902:444", out _) &&
                !points.TryGetProperty("8:", out _) && !points.TryGetProperty("0:555", out _),
                "point identity is stateId:pointId, and an unknown scene never becomes a record");
        }

        // 4. The sources are byte for byte what they were.
        check(File.ReadAllBytes(legacyPath).SequenceEqual(legacyBytes),
            "recovering a pre-rewrite file leaves that file exactly as it was");

        // 5. Running it again does not duplicate anything.
        int before = catalog.Accounts.Count;
        var second = recovery.Recover(catalog);
        check(catalog.Accounts.Count == before && second.Recovered.Count == 0,
            "a second recovery creates no second record book for the same source");
        check(recovery.Scan(catalog.Accounts.Select(account => account.Id).ToArray())
                .All(source => source.AlreadyRecovered || !source.Recoverable),
            "after recovering, every remaining source is reported as already recovered or unusable");

        // 6. The journal is plain JSON the support workflow can read.
        check(File.Exists(recovery.JournalPath) && File.ReadAllText(recovery.JournalPath).Contains("account_1.json"),
            "what was recovered, and from where, is written down next to the record books");

        // 7. A source with nothing usable is reported, not turned into an empty record book.
        //    Its program directory sits in a parent of its own, so the neighbour scan has nothing to
        //    find and this case really only sees the one file below.
        string emptyRoot = Path.Combine(directory, "empty");
        string emptyProgram = Path.Combine(emptyRoot, "prog", "IMao");
        Directory.CreateDirectory(emptyProgram);
        Directory.CreateDirectory(Path.Combine(emptyRoot, "SavedPoints"));
        File.WriteAllText(Path.Combine(emptyRoot, "SavedPoints", "account_1.json"), "{\"Unknown\":{\"x\":[{\"id\":\"1\"}]}}");
        var emptyCatalog = new LocalAccountCatalog(Path.Combine(emptyRoot, "SavedPoints", "accounts.json"),
            Path.Combine(emptyRoot, "SavedPoints", "profiles"));
        var emptyRecovery = new LegacyPointRecovery(Path.Combine(emptyRoot, "SavedPoints"), emptyProgram);
        var emptyReport = emptyRecovery.Recover(emptyCatalog);
        check(emptyReport.Recovered.Count == 0 && emptyCatalog.Accounts.Count == 1 &&
            emptyRecovery.Scan(emptyCatalog.Accounts.Select(account => account.Id).ToArray())
                .Count(source => !source.Recoverable && source.Problem.Length > 0) == 1,
            "a file whose scenes are all unknown is reported with a reason instead of becoming an empty record book");
    }
}
