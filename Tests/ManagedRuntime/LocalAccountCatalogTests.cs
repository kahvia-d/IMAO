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
