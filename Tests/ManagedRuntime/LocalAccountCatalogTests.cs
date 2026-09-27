using IMao_WinUI.Services;
using System.Text;

internal static class LocalAccountCatalogTests
{
    /// <summary>
    /// The ledger list is the player's own inventory of local progress. Seeding it must
    /// never move, merge or delete anything, and a file that cannot be read must be left
    /// exactly as it was. See Docs/LocalAccounts_20260926.md sections 4.1 and 5.
    /// </summary>
    public static void Run(string root, Action<bool, string> check)
    {
        string directory = Path.Combine(root, "account-catalog-" + Guid.NewGuid().ToString("N"));

        // A fresh installation: one ledger, no binding, written once so it can be edited.
        string freshRoot = Path.Combine(directory, "fresh", "SavedPoints");
        string freshPath = Path.Combine(freshRoot, "accounts.json");
        var fresh = new LocalAccountCatalog(freshPath, Path.Combine(freshRoot, "profiles"));
        check(fresh.Accounts.Count == 1 && fresh.ActiveId == "local" && fresh.Active.Name == "默认" &&
            !fresh.Active.IsBound && fresh.Warning.Length == 0,
            "a fresh installation starts with one unbound default ledger and no warning");
        check(File.Exists(freshPath) && File.ReadAllText(freshPath).Contains("\"activeAccountId\"") &&
            File.ReadAllText(freshPath).Contains("\"kuroAccountId\""),
            "the seeded ledger list is written once with the documented field names");
        var freshReload = new LocalAccountCatalog(freshPath, Path.Combine(freshRoot, "profiles"));
        check(freshReload.Accounts.Count == 1 && freshReload.ActiveId == "local" && freshReload.Warning.Length == 0,
            "the seeded list round trips without a warning");

        // The settings list needs per-ledger counts, and describing one must never throw.
        string describedProfiles = Path.Combine(directory, "describe", "profiles");
        Directory.CreateDirectory(describedProfiles);
        File.WriteAllText(Path.Combine(describedProfiles, "local.json"),
            "{\"schemaVersion\":2,\"profileId\":\"local\",\"points\":{\"8:a\":{\"completed\":true}," +
            "\"8:b\":{\"completed\":false},\"8:c\":{\"completed\":true}}}");
        var describing = new LocalAccountCatalog(Path.Combine(directory, "describe", "accounts.json"), describedProfiles);
        var summary = describing.Describe("local");
        check(summary.Completed == 2 && summary.Total == 3 && summary.WrittenAt is not null,
            "a ledger reports how much progress it holds");
        var missingSummary = describing.Describe("missing-ledger");
        check(missingSummary.Completed == 0 && missingSummary.Total == 0 && missingSummary.WrittenAt is null &&
            describing.Describe("../escape").Total == 0,
            "describing an unknown or invalid ledger reports empty instead of failing");
        File.WriteAllText(Path.Combine(describedProfiles, "local.json"), "not json at all");
        check(describing.Describe("local").Total == 0,
            "a damaged progress document is reported as empty instead of failing the settings page");

        // The report a player can send answers "which ledger holds the data" without guessing.
        string report = describing.WriteDiagnostics();
        string reportText = File.ReadAllText(report);
        check(File.Exists(report) && reportText.Contains("\"activeAccountId\"") && reportText.Contains("\"completed\"") &&
            reportText.Contains("\"hasCredential\"") && !reportText.Contains("secret"),
            "the ledger report lists every ledger and carries no credential");

        // A credential belongs to the Kuro account, so "does this record book have one" is
        // answered by its binding, not by its id. The report a player sends has to say the same
        // thing the synchronization does, or support chases a file that was never going to be read.
        string credentialRoot = Path.Combine(directory, "credential", "SavedPoints");
        string credentialStore = Path.Combine(directory, "credential", "KuroSync", "credentials");
        Directory.CreateDirectory(credentialStore);
        var bound = new LocalAccountCatalog(Path.Combine(credentialRoot, "accounts.json"),
            Path.Combine(credentialRoot, "profiles"), credentialsDirectory: credentialStore);
        check(bound.CredentialFile("local").Length == 0, "an unbound record book has no credential to find");
        check(bound.TryBind("local", "10146974", out _), "the default record book can be bound to an account");
        check(bound.CredentialFile("local").Length == 0,
            "binding an account does not invent a credential that was never connected");
        File.WriteAllText(Path.Combine(credentialStore, "kuro_10146974.json"), "{\"Ciphertext\":\"x\"}");
        check(bound.CredentialFile("local") == "kuro_10146974.json",
            "a record book bound to an account finds the credential stored under that account");
        string credentialReport = File.ReadAllText(bound.WriteDiagnostics());
        check(credentialReport.Contains("\"hasCredential\": true") &&
            credentialReport.Contains("kuro_10146974.json") && credentialReport.Contains("\"storedCredentialAccounts\""),
            "the ledger report names the credential file it found and the accounts this machine holds");
        check(bound.TryCreate("第二本", "10436687", out var second, out _) && second is not null,
            "a second record book for another account can be created");
        check(!bound.SyncStateIds(second!.Id).Any() && !bound.SyncStateIds("../escape").Any(),
            "reading the baseline regions of an empty or invalid record book reports none");
        check(bound.TrySetActive(second.Id, out _),
            "the second record book becomes the current one");
        check(bound.TryDelete(second.Id, out _) && File.Exists(Path.Combine(credentialStore, "kuro_10146974.json")),
            "deleting a record book leaves credentials alone, because they belong to the account");

        // 删除 → 恢复 → 删除 would otherwise park one identical folder per round, and the folder name
        // is a timestamp, so nothing would ever collapse them again. Identical means the same id,
        // name and binding, the same progress bytes and the same routes.
        string archiveRoot = Path.Combine(directory, "archive");
        string archiveProfiles = Path.Combine(archiveRoot, "profiles");
        string archiveRoutes = Path.Combine(archiveRoot, "SavedRoutes", "Auto");
        Directory.CreateDirectory(archiveProfiles);
        string parkedProgressPath = Path.Combine(archiveProfiles, "local.json");
        File.WriteAllText(parkedProgressPath, "{\"schemaVersion\":2,\"profileId\":\"local\",\"revision\":1,\"points\":{},\"syncStates\":[]}");
        Directory.CreateDirectory(Path.Combine(archiveRoutes, "local"));
        File.WriteAllText(Path.Combine(archiveRoutes, "local", "active.json"), "{\"formatVersion\":1,\"routeId\":null}");
        var archive = new LocalAccountCatalog(Path.Combine(archiveRoot, "accounts.json"), archiveProfiles,
            routesDirectory: archiveRoutes);
        check(archive.TryRename("local", "主号", out _) && archive.TryBind("local", "10383865", out _) &&
            archive.TryCreate("备用", "", out _, out _),
            "the record book that gets deleted twice is named and bound first");
        byte[] parkedProgress = File.ReadAllBytes(parkedProgressPath);

        check(archive.TryDelete("local", out _, out string firstNote) && firstNote.Length == 0 &&
            Directory.GetDirectories(archive.DeletedDirectory).Length == 1 &&
            File.Exists(Path.Combine(Directory.GetDirectories(archive.DeletedDirectory)[0], "ledger.json")),
            "deleting a record book parks it once, with a manifest that names it");

        void Restore(string restoredName, string restoredBinding, byte[] bytes)
        {
            File.WriteAllBytes(parkedProgressPath, bytes);
            Directory.CreateDirectory(Path.Combine(archiveRoutes, "local"));
            File.WriteAllText(Path.Combine(archiveRoutes, "local", "active.json"), "{\"formatVersion\":1,\"routeId\":null}");
            archive.TryAdopt("local", restoredName, restoredBinding, out _, out _);
        }

        Restore("主号", "10383865", parkedProgress);
        check(archive.TryDelete("local", out _, out string secondNote) && secondNote.Length > 0 &&
            Directory.GetDirectories(archive.DeletedDirectory).Length == 1,
            "deleting the very same record book again keeps the one copy and says so instead of parking a twin");

        Restore("改了个名", "10383865", parkedProgress);
        check(archive.TryDelete("local", out _, out _) && Directory.GetDirectories(archive.DeletedDirectory).Length == 2,
            "a record book whose name changed is a different record book, so its copy is kept as well");

        Restore("改了个名", "10383865", Encoding.UTF8.GetBytes(
            "{\"schemaVersion\":2,\"profileId\":\"local\",\"revision\":9,\"points\":{\"8:new\":{\"completed\":true}},\"syncStates\":[]}"));
        check(archive.TryDelete("local", out _, out _) && Directory.GetDirectories(archive.DeletedDirectory).Length == 3,
            "a record book whose progress changed is a different state, so its copy is kept as well");

        int beforeEmpty = Directory.GetDirectories(archive.DeletedDirectory).Length;
        check(archive.TryCreate("空本", "", out var neverWritten, out _) && neverWritten is not null &&
            archive.TryDelete(neverWritten.Id, out _, out string nothingNote) && nothingNote.Length > 0 &&
            Directory.GetDirectories(archive.DeletedDirectory).Length == beforeEmpty,
            "deleting a record book that never wrote a document creates no folder at all");

        // The regions a rebind has to drop come from the record book's own document.
        string baselineProfiles = Path.Combine(directory, "baseline", "profiles");
        Directory.CreateDirectory(baselineProfiles);
        File.WriteAllText(Path.Combine(baselineProfiles, "local.json"),
            "{\"schemaVersion\":2,\"profileId\":\"local\",\"points\":{},\"syncStates\":[" +
            "{\"stateId\":8,\"initialized\":true},{\"stateId\":903,\"initialized\":false},{\"stateId\":0}]}");
        var baseline = new LocalAccountCatalog(Path.Combine(directory, "baseline", "accounts.json"), baselineProfiles);
        check(baseline.SyncStateIds("local").SequenceEqual(new[] { 8, 903 }),
            "the regions a record book holds a cloud baseline for are read from its own document");

        // The upgrade path: progress files plus the historical account metadata.
        string upgrade = Path.Combine(directory, "upgrade");
        string profiles = Path.Combine(upgrade, "profiles");
        Directory.CreateDirectory(profiles);
        File.WriteAllText(Path.Combine(profiles, "kuro_10383865.json"), "{}");
        File.WriteAllText(Path.Combine(profiles, "local.json"), "{}");
        string legacyPath = Path.Combine(upgrade, "kuromap-accounts.json");
        File.WriteAllText(legacyPath, "{\"ActiveProfile\":\"kuro_10383865\"}");
        string catalogPath = Path.Combine(upgrade, "accounts.json");
        var upgraded = new LocalAccountCatalog(catalogPath, profiles, legacyPath);
        check(upgraded.Accounts.Count == 2 && upgraded.ActiveId == "kuro_10383865" && upgraded.Warning.Length == 0,
            "the historical selection becomes the active ledger of the seeded list");
        var accountLedger = upgraded.Accounts.Single(account => account.Id == "kuro_10383865");
        check(accountLedger.KuroAccountId == "10383865" && accountLedger.Name == "库街区 10383865",
            "an account ledger keeps its binding and gets a readable name");
        check(!upgraded.Accounts.Single(account => account.Id == "local").IsBound,
            "the local ledger stays unbound");
        check(File.Exists(Path.Combine(profiles, "kuro_10383865.json")) && File.Exists(Path.Combine(profiles, "local.json")),
            "seeding never moves or deletes a progress file");

        // Selecting is the player's decision and survives a restart.
        check(upgraded.TrySetActive("local", out _), "a listed ledger can be selected");
        check(new LocalAccountCatalog(catalogPath, profiles, legacyPath).ActiveId == "local",
            "the selected ledger survives a restart");
        check(upgraded.TrySetActive("kuro_777", out _) && upgraded.ActiveId == "kuro_777" &&
            upgraded.Accounts.Any(account => account.Id == "kuro_777" && account.KuroAccountId == "777") &&
            new LocalAccountCatalog(catalogPath, profiles, legacyPath).ActiveId == "kuro_777",
            "selecting a ledger that was not listed yet adopts it instead of hiding it");
        check(!upgraded.TrySetActive("../escape", out _) && upgraded.ActiveId == "kuro_777",
            "an id that cannot be a file name is refused");
        check(File.ReadAllText(legacyPath).Contains("\"kuro_777\""),
            "the old account metadata keeps naming the ledger the player selected, so a rolled-back version shows it too");
        File.WriteAllText(legacyPath, "{\"ActiveProfile\":\"kuro_10383865\",\"AutomaticSync\":true,\"Accounts\":[{\"UserId\":\"1\"}]}");
        var rewritten = new LocalAccountCatalog(catalogPath, profiles, legacyPath);
        rewritten.TrySetActive("local", out _);
        string afterLegacy = File.ReadAllText(legacyPath);
        check(afterLegacy.Contains("\"AutomaticSync\"") && afterLegacy.Contains("\"Accounts\"") && afterLegacy.Contains("\"local\""),
            "updating the old selection preserves every other field in that file");

        // Managing ledgers.
        var managed = new LocalAccountCatalog(Path.Combine(directory, "manage", "accounts.json"),
            Path.Combine(directory, "manage", "profiles"));
        check(!managed.TryCreate("", "", out _, out _) && !managed.TryCreate(new string('x', 41), "", out _, out _) &&
            managed.Accounts.Count == 1,
            "a ledger name is required and bounded");
        check(managed.TryCreate("小号", "10436687", out var created, out _) && created is { IsBound: true } &&
            created.Id.StartsWith("acc_") && managed.Accounts.Count == 2,
            "a new ledger can be created with a Kuro binding");
        check(!managed.TryBind("local", "10436687", out _),
            "one Kuro account cannot be bound to two ledgers");
        check(managed.TryBind("local", "10436687", out _) == false && managed.TryBind("local", "10383865", out _) &&
            managed.Accounts.Single(account => account.Id == "local").KuroAccountId == "10383865" &&
            managed.TryBind("local", "", out _) && !managed.Accounts.Single(account => account.Id == "local").IsBound,
            "a ledger can be bound, rebound and unbound");
        check(!managed.TryBind("local", "abc", out _) && !managed.TryBind("local", new string('9', 25), out _),
            "a binding must be digits within the documented length or empty");
        // The extension popup prints "kuro_<id>" (every build published before 2026-09-27 prints
        // nothing else), so pasting it back has to mean the same account instead of being refused.
        check(LocalAccountCatalog.NormalizeKuroAccount("kuro_10383865") == "10383865" &&
            LocalAccountCatalog.NormalizeKuroAccount("  KURO_10383865 ") == "10383865" &&
            LocalAccountCatalog.NormalizeKuroAccount("10383865") == "10383865" &&
            LocalAccountCatalog.NormalizeKuroAccount("kuro_") == "" &&
            LocalAccountCatalog.NormalizeKuroAccount("") == "" && LocalAccountCatalog.NormalizeKuroAccount(null) == "",
            "an account id copied out of the extension popup is accepted with or without its prefix");
        check(managed.TryBind("local", LocalAccountCatalog.NormalizeKuroAccount("kuro_10383865"), out _) &&
            managed.Accounts.Single(account => account.Id == "local").KuroAccountId == "10383865" &&
            managed.TryBind("local", "", out _),
            "the binding stores the digits alone, never the displayed form");
        check(!managed.TryRename(created!.Id, "  ", out _) && managed.TryRename(created.Id, "备用号", out _) &&
            managed.Accounts.Single(account => account.Id == created.Id).Name == "备用号" &&
            managed.TryCreate("重复绑定", "10436687", out _, out _) == false,
            "renaming changes the display name and never the id, and the binding stays unique");

        var capped = new LocalAccountCatalog(Path.Combine(directory, "cap", "accounts.json"),
            Path.Combine(directory, "cap", "profiles"));
        int createdCount = 0;
        for (int index = 0; index < LocalAccountCatalog.MaximumAccounts + 4; ++index)
            if (capped.TryCreate("账本" + index, "", out _, out _)) ++createdCount;
        check(createdCount == LocalAccountCatalog.MaximumAccounts - 1 &&
            capped.Accounts.Count == LocalAccountCatalog.MaximumAccounts,
            "the ledger list stops at its documented bound");

        // A list that cannot be understood is never rewritten, not even by an edit.
        string brokenPath = Path.Combine(directory, "broken", "accounts.json");
        Directory.CreateDirectory(Path.GetDirectoryName(brokenPath)!);
        byte[] broken = Encoding.UTF8.GetBytes(
            "{\"version\":1,\"activeAccountId\":\"local\",\"accounts\":[" +
            "{\"id\":\"local\",\"name\":\"默认\",\"kuroAccountId\":\"\"}," +
            "{\"id\":\"local\",\"name\":\"重复\",\"kuroAccountId\":\"\"}]}");
        File.WriteAllBytes(brokenPath, broken);
        var brokenCatalog = new LocalAccountCatalog(brokenPath, Path.Combine(directory, "broken", "profiles"));
        check(brokenCatalog.Warning.Length > 0 && brokenCatalog.ActiveId == "local" && brokenCatalog.Accounts.Count == 1,
            "a duplicated ledger list falls back to the default ledger with a warning");
        check(File.ReadAllBytes(brokenPath).SequenceEqual(broken), "an unreadable ledger list is never rewritten");
        brokenCatalog.TryCreate("新账本", "", out _, out _);
        check(File.ReadAllBytes(brokenPath).SequenceEqual(broken),
            "even an edit never rewrites a ledger list that could not be read");

        File.WriteAllText(brokenPath, "{\"version\":1,\"activeAccountId\":\"local\",\"accounts\":[{\"id\":\"local\",\"name\":\"" +
            new string('x', LocalAccountCatalog.MaximumBytes) + "\",\"kuroAccountId\":\"\"}]}");
        check(new LocalAccountCatalog(brokenPath, Path.Combine(directory, "broken", "profiles")).Warning.Length > 0,
            "an oversized ledger list is refused");

        File.WriteAllText(brokenPath, "{\"version\":2,\"activeAccountId\":\"local\",\"accounts\":[{\"id\":\"local\",\"name\":\"默认\",\"kuroAccountId\":\"\"}]}");
        check(new LocalAccountCatalog(brokenPath, Path.Combine(directory, "broken", "profiles")).Warning.Length > 0,
            "an unknown ledger list version is refused instead of guessed");
    }
}
