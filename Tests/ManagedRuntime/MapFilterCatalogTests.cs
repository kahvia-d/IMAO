using IMao_WinUI.Models;

internal static class MapFilterCatalogTests
{
    public static void Run(string testRoot, Action<bool, string> check)
    {
        string root = Path.Combine(testRoot, "catalog-test");
        Directory.CreateDirectory(Path.Combine(root, "catalogs"));
        Directory.CreateDirectory(Path.Combine(root, "icons"));
        File.WriteAllText(Path.Combine(root, "icons", "item.png"), "fixture");
        File.WriteAllText(Path.Combine(root, "catalogs", "catalog-8.json"), """
            [
              {"id":"1","name":"角色","children":[
                {"id":"character","name":"测试角色","acronym":"jueshe","children":[{"id":"a"},{"id":"blocked"},{"id":"a"}]}]},
              {"id":"2","name":"武器","children":[
                {"id":"weapon","name":"测试武器","children":[{"id":"a"},{"id":"sx·qq"}]}]},
              {"id":"8","name":"挑战","children":[{"id":"a","name":"官方中文名"}]},
              {"id":"3","name":"收集物","children":[{"id":"sx·qq","name":"声匣·七丘"}]},
              {"id":"6","name":"敌人","children":[{"id":"blocked","name":"未审批敌人"}]}
            ]
            """);
        File.WriteAllText(Path.Combine(root, "catalogs", "catalog-900.json"), """
            [
              {"id":"8","name":"BOSS","children":[{"id":"b","name":"另一地区首领"}]},
              {"id":"1","name":"角色","children":[
                {"id":"character","name":"测试角色","children":[{"id":"b"},{"id":"a"}]}]}
            ]
            """);
        File.WriteAllText(Path.Combine(root, "catalogs", "catalog-broken.json"), "broken");
        File.WriteAllText(Path.Combine(root, "icon-manifest.json"), """
            {"icons":{"a":"icons/item.png","b":"../outside.png","sx_qq":"icons/missing.png"}}
            """);
        File.WriteAllText(Path.Combine(testRoot, "outside.png"), "outside fixture");
        MapFilterSourceItem[] available =
        [
            new("a", "Existing English Name", "3.0 new"),
            new("b", "旧首领名", "Enemy"),
            new("sx_qq", "声匣·七丘", "Chest"),
            new("unknown", "兼容旧类型", "legacy"),
            new("a", "Later Duplicate", "wrong")
        ];
        // The fixture keeps catalogs and icons in one directory, which is the pre-split layout; the icon
        // package root is passed explicitly because the running snapshot is not what a test is testing.
        MapFilterCatalog catalog = MapFilterCatalog.Load(available, root, root);
        check(catalog.Items.Count == 4 && catalog.Items.Select(item => item.Id).Distinct().Count() == 4,
            "filter catalog retains every available point ID exactly once");
        check(catalog.Categories.SelectMany(group => group.Items).Select(item => item.Id).ToHashSet()
            .SetEquals(available.Select(item => item.Id)), "category browsing preserves all prior switches");
        check(catalog.Categories.Select(group => group.Name).SequenceEqual(["收集物", "挑战", "BOSS", "补充分类"]),
            "official category names distinguish changing state IDs and retain official order");
        MapFilterItem item = catalog.Items.Single(item => item.Id == "a");
        check(item.Name == "Existing English Name" && item.OfficialName == "官方中文名" &&
            item.Matches("english") && item.Matches("中文") && item.Matches("3.0 new"),
            "filter search supports old translations and official Chinese names");
        check(catalog.Items.Single(item => item.Id == "sx_qq").Category == "收集物",
            "official middle-dot IDs retain established runtime aliases");
        check(catalog.CharacterGroups.Count == 1 && catalog.CharacterGroups[0].Items.Select(item => item.Id).SequenceEqual(["a", "b"])
            && catalog.WeaponGroups.Single().Items.Select(item => item.Id).SequenceEqual(["a", "sx_qq"]),
            "character and weapon shortcuts merge states without admitting unavailable point IDs");
        check(ReferenceEquals(item, catalog.CharacterGroups[0].Items[0]) && ReferenceEquals(item, catalog.WeaponGroups[0].Items[0]),
            "all shortcut occurrences share the same point item");
        check(catalog.CharacterGroups[0].Matches("jueshe") && catalog.CharacterGroups[0].Matches("官方"),
            "shortcut search includes character acronym and referenced materials");
        check(item.IconPath == Path.Combine(root, "icons", "item.png") && catalog.Items.Single(item => item.Id == "b").IconPath is null
            && catalog.Items.Single(item => item.Id == "sx_qq").IconPath is null,
            "filter icons only use existing files inside the published map directory");
        check(catalog.HasOfficialCatalog && catalog.LastError.Contains("catalog-broken.json"),
            "one damaged catalog reports a warning while other states remain usable");

        MapFilterCatalog missing = MapFilterCatalog.Load(available, Path.Combine(testRoot, "missing-catalogs"), Path.Combine(testRoot, "missing-catalogs"));
        check(!missing.HasOfficialCatalog && missing.Items.Count == 4 && missing.Categories.Single().Name == "补充分类" &&
            missing.LastError.Length > 0 && missing.CharacterGroups.Count == 0,
            "missing offline catalogs preserve all filters and expose a warning");

        string damagedRoot = Path.Combine(testRoot, "all-damaged-catalogs");
        Directory.CreateDirectory(Path.Combine(damagedRoot, "catalogs"));
        File.WriteAllText(Path.Combine(damagedRoot, "catalogs", "catalog-8.json"), "{}");
        File.WriteAllText(Path.Combine(damagedRoot, "icon-manifest.json"), "invalid");
        MapFilterCatalog damaged = MapFilterCatalog.Load(available, damagedRoot, damagedRoot);
        check(damaged.Items.Count == 4 && damaged.Categories.Single().Items.Count == 4 && damaged.Warnings.Count == 2,
            "damaged metadata and icons never remove available point types");
    }
}
