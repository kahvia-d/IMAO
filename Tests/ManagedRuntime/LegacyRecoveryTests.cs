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
        // The pre-rewrite version kept the player's own account names here; a record book deleted from
        // the list has lost the name it was given, so this is the only place an old name survives.
        File.WriteAllText(Path.Combine(directory, "kuromap-accounts.json"),
            "{\"ActiveProfile\":\"kuro_777\",\"Accounts\":[{\"UserId\":\"777\",\"DisplayName\":\"Theyun\"}]}");

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
        var scan = recovery.Scan(catalog.Accounts);
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

        // The row has to say which record book it is, and for an old account the name the player
        // chose back then is the most recognisable thing about it.
        check(singles.All(source => source.LedgerName == "旧数据恢复" && source.DisplayName == "旧数据恢复"),
            "a pre-rewrite file reports the name of the record book it will become");
        check(scan.Single(source => source.Kind == "deleted-document").LedgerName == "默认",
            "a deleted 默认 document reports the name it will get back");
        check(scan.Single(source => source.LedgerId == "kuro_777").LedgerName == "库街区 777" &&
            scan.Single(source => source.LedgerId == "kuro_777").DisplayName == "库街区 777（旧版叫 Theyun）",
            "an unlisted document shows its record-book name plus the old name the legacy metadata still holds");

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
        check(recovery.Scan(catalog.Accounts)
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
            emptyRecovery.Scan(emptyCatalog.Accounts)
                .Count(source => !source.Recoverable && source.Problem.Length > 0) == 1,
            "a file whose scenes are all unknown is reported with a reason instead of becoming an empty record book");

        // 8. The player chooses. A machine can hold a record book from an account they no longer want
        //    on it, which is exactly why the list is tickable instead of "recover everything found".
        string choicePoints = Path.Combine(directory, "choice", "SavedPoints");
        string choiceProgram = Path.Combine(directory, "choice", "prog", "IMao");
        Directory.CreateDirectory(choicePoints);
        Directory.CreateDirectory(choiceProgram);
        var choiceCatalog = new LocalAccountCatalog(Path.Combine(choicePoints, "accounts.json"), Path.Combine(choicePoints, "profiles"));
        Directory.CreateDirectory(Path.Combine(choicePoints, "profiles"));
        foreach (string account in new[] { "kuro_555", "kuro_666" })
            File.WriteAllText(Path.Combine(choicePoints, "profiles", account + ".json"),
                "{\"schemaVersion\":2,\"profileId\":\"" + account + "\",\"revision\":1,\"syncStates\":[],\"points\":{" +
                "\"8:p\":{\"sceneName\":\"World\",\"nameId\":\"cx_01\",\"stateId\":8,\"pointId\":\"p\",\"completed\":true," +
                "\"remoteCompleted\":null,\"pending\":false}}}");
        var choiceRecovery = new LegacyPointRecovery(choicePoints, choiceProgram);
        byte[] untickedBytes = File.ReadAllBytes(Path.Combine(choicePoints, "profiles", "kuro_666.json"));
        var choiceScan = choiceRecovery.Scan(choiceCatalog.Accounts);
        check(choiceScan.Count == 2, "the scan offers both unlisted documents as separate rows");
        var chosen = choiceRecovery.Recover(choiceCatalog, [choiceScan.Single(source => source.LedgerId == "kuro_555")]);
        check(chosen.Recovered.Count == 1 && choiceCatalog.Accounts.Any(account => account.Id == "kuro_555") &&
            !choiceCatalog.Accounts.Any(account => account.Id == "kuro_666") &&
            File.ReadAllBytes(Path.Combine(choicePoints, "profiles", "kuro_666.json")).SequenceEqual(untickedBytes),
            "only the ticked finding is recovered; the one left unticked is neither listed nor altered");
        var remaining = choiceRecovery.Scan(choiceCatalog.Accounts);
        check(remaining.Count == 1 && remaining[0].LedgerId == "kuro_666" && remaining[0].Recoverable,
            "a finding the player left unticked is offered again next time");
        check(choiceRecovery.Recover(choiceCatalog, remaining).Recovered.Count == 1 &&
            choiceCatalog.Accounts.Any(account => account.Id == "kuro_666"),
            "the remaining finding is recovered once it is ticked");

        // A parked record book carries a manifest with the name and binding it had — the only place
        // those survive, because they lived in the list entry 删除 removed. Restoring it brings the
        // record book back as it was, and the manifest is never offered as point data.
        string parked = Path.Combine(choicePoints, "deleted", "20260927-120000-acc_abc12345");
        Directory.CreateDirectory(parked);
        File.WriteAllText(Path.Combine(parked, "ledger.json"),
            "{\"version\":1,\"id\":\"acc_abc12345\",\"name\":\"我的小号\",\"kuroAccountId\":\"10383865\"," +
            "\"deletedAtUtc\":\"2026-09-27T04:00:00+00:00\",\"routes\":false}");
        File.WriteAllText(Path.Combine(parked, "acc_abc12345.json"),
            "{\"schemaVersion\":2,\"profileId\":\"acc_abc12345\",\"revision\":4,\"syncStates\":[],\"points\":{" +
            "\"8:r\":{\"sceneName\":\"World\",\"nameId\":\"cx_01\",\"stateId\":8,\"pointId\":\"r\",\"completed\":true," +
            "\"remoteCompleted\":null,\"pending\":false}}}");
        var parkedScan = choiceRecovery.Scan(choiceCatalog.Accounts);
        var parkedRow = parkedScan.Single(source => source.Kind == "deleted-document");
        check(parkedScan.Count(source => source.Kind == "deleted-document") == 1 && parkedRow.Points == 1 &&
            parkedRow.LedgerName == "我的小号" && parkedRow.LedgerBinding == "10383865",
            "a parked record book is listed under the name it had, and the manifest beside it is not offered as data");
        check(choiceRecovery.Recover(choiceCatalog, [parkedRow]).Recovered.Count == 1 &&
            choiceCatalog.Accounts.Any(account => account.Id == "acc_abc12345" &&
                account.Name == "我的小号" && account.KuroAccountId == "10383865"),
            "restoring a parked record book brings back the name and the account it was bound to");

        // The row a player actually saw: a pre-rewrite file, already brought in, unselectable. It has
        // to say WHICH record book holds the points — and it must become selectable again once that
        // record book is gone, because then the data really is stranded again. A journal entry alone
        // is not "already recovered"; the record book still being there is.
        string replayPoints = Path.Combine(directory, "replay", "SavedPoints");
        string replayProgram = Path.Combine(directory, "replay", "prog", "IMao");
        Directory.CreateDirectory(replayPoints);
        Directory.CreateDirectory(replayProgram);
        File.WriteAllText(Path.Combine(replayPoints, "account_1.json"), "{\"World\":{\"cx_01\":[{\"id\":\"777777\"}]}}");
        var replayCatalog = new LocalAccountCatalog(Path.Combine(replayPoints, "accounts.json"), Path.Combine(replayPoints, "profiles"));
        var replay = new LegacyPointRecovery(replayPoints, replayProgram);
        var firstReplayScan = replay.Scan(replayCatalog.Accounts);
        check(firstReplayScan.Count == 1 && !firstReplayScan[0].AlreadyRecovered && firstReplayScan[0].Recoverable,
            "a pre-rewrite file that has never been brought in is offered for recovery");
        var replayReport = replay.Recover(replayCatalog, firstReplayScan);
        var secondReplayScan = replay.Scan(replayCatalog.Accounts);
        check(replayReport.Recovered.Count == 1 && secondReplayScan.Count == 1 &&
            secondReplayScan[0].AlreadyRecovered && secondReplayScan[0].RecoveredInto == replayReport.Recovered[0].LedgerName,
            "once its points are in a record book, the same file says which record book holds them");
        check(replayCatalog.TryDelete(replayReport.Recovered[0].LedgerId, out _), "the record book that holds them can be deleted");
        // Deleting the record book parks its document, so the data is now reachable two ways: from the
        // pre-rewrite file it came from, and from the copy under deleted/. Both must be offered.
        var thirdReplayScan = replay.Scan(replayCatalog.Accounts);
        check(thirdReplayScan.Count(source => source.Kind == "pre-rewrite-record") == 1 &&
            thirdReplayScan.Single(source => source.Kind == "pre-rewrite-record") is { AlreadyRecovered: false, Recoverable: true } &&
            thirdReplayScan.Count(source => source.Kind == "deleted-document") == 1,
            "after that record book is deleted the same file is offered again, and so is the copy 删除 parked");

        // A recovery that cannot write its document must leave no half-done record book behind: the
        // record book it had already added to the list is taken back out.
        string stuckPoints = Path.Combine(directory, "stuck", "SavedPoints");
        Directory.CreateDirectory(stuckPoints);
        File.WriteAllText(Path.Combine(stuckPoints, "account_1.json"), "{\"World\":{\"cx_01\":[{\"id\":\"888888\"}]}}");
        File.WriteAllText(Path.Combine(stuckPoints, "profiles"), "not a directory");
        var stuckCatalog = new LocalAccountCatalog(Path.Combine(stuckPoints, "accounts.json"), Path.Combine(stuckPoints, "profiles"));
        var stuck = new LegacyPointRecovery(stuckPoints, Path.Combine(directory, "stuck", "prog", "IMao"));
        var stuckReport = stuck.Recover(stuckCatalog);
        check(stuckReport.Recovered.Count == 0 && stuckCatalog.Accounts.Count == 1 && stuckReport.Notes.Count > 0 &&
            stuck.Scan(stuckCatalog.Accounts).Single().Recoverable,
            "a recovery that cannot write its document takes the new record book back out and says why");
    }
}
