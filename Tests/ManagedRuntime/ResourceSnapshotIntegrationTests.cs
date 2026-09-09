using System.Reflection;
using System.Security.Cryptography;
using IMao_WinUI.Core.Updates;
using IMao_WinUI.Helpers;
using IMao_WinUI.Models;
using IMao_WinUI.Services;
using IMao_WinUI.StringItems;

internal static class ResourceSnapshotIntegrationTests
{
    public static async Task RunAsync(string testRoot, Action<bool, string> check)
    {
        var before = new StringItem(); before.LoadString("zh-CN");
        check(before.itemsDatas.SelectMany(c => c.ItemDatas).Any(i => i.Id == "sx" && i.Name_SpecifiedLanguage == "声匣"),
            "bundled catalog retains embedded translations before a resource session is selected");
        check(before.itemsDatas.SelectMany(c => c.ItemDatas).Any(i => i.Id == "sx_qq"), "embedded removed-item fixture exists in shipped translations");

        var root = Path.Combine(testRoot, "resource-integration");
        var cache = Path.Combine(root, "ResourceUpdates");
        var data = Path.Combine(cache, "packages", "map-data", "2026.9.9.2");
        Directory.CreateDirectory(data);
        var contents = new Dictionary<string, string>
        {
            ["filter-items.json"] = """{"Chest":{"sx":{"zh-CN":"新版声匣","en-US":"Updated Sonance"},"external-item":{"zh-CN":"新物件","en-US":"New item"}}}""",
            ["new-state-filter-items.json"] = """{"New":{"approved-item":{"zh-CN":"已验证地区物件"},"blocked-item":{"zh-CN":"未验证地区物件"},"sx_qq":{"zh-CN":"不应恢复的旧物件"}}}""",
            ["scene-validation.json"] = """{"scenes":{"ApprovedScene":{"approved":true},"BlockedScene":{"approved":false}}}""",
            ["new-state-item-scenes.json"] = """{"items":{"approved-item":["ApprovedScene"],"blocked-item":["BlockedScene"],"sx_qq":["BlockedScene"]}}"""
        };
        var files = new List<ResourceFile>();
        foreach (var pair in contents)
        {
            var path = Path.Combine(data, pair.Key); await File.WriteAllTextAsync(path, pair.Value);
            var bytes = File.ReadAllBytes(path);
            files.Add(new ResourceFile { Path = pair.Key, Size = bytes.Length, Sha256 = Convert.ToHexString(SHA256.HashData(bytes)) });
        }
        var bundled = new ResourceSnapshot { Bundled = true, SnapshotId = "integration-bundled", BaselineId = "integration-baseline", BaselineRoot = Path.Combine(root, "Assets"), MapDataRoot = Path.Combine(root, "Assets/KuroMap") };
        var first = new ResourceSnapshotService(cache, bundled, "2026.9.9.2", (_, _) => Task.CompletedTask); await first.InitializeAsync();
        var installed = new ResourceSnapshot
        {
            FormatVersion = 2, MinAppVersion = "2026.9.9.2",
            SnapshotId = "integration-installed", Sequence = 2, BaselineId = bundled.BaselineId, BaselineRoot = bundled.BaselineRoot, MapDataRoot = data,
            Packages = [new SnapshotPackage { Id = "map-data", Version = "2026.9.9.2", Kind = "map-data", Directory = data, Files = files, Sha256 = new string('A', 64) }]
        };
        await using (var held = await UpdateStorage.LockAsync(cache, CancellationToken.None)) await first.StageAsync(installed, CancellationToken.None);
        var session = new ResourceSnapshotService(cache, bundled, "2026.9.9.2", (_, _) => Task.CompletedTask); await session.InitializeAsync();
        ResourceSessionPaths.Initialize(session);
        check(ResourceSessionPaths.MapDataRoot == data && !session.Current.Bundled, "frontend map root follows the complete installed snapshot");

        var strings = new StringItem(); strings.LoadString("zh-CN");
        var items = strings.itemsDatas.SelectMany(c => c.ItemDatas).ToDictionary(i => i.Id, i => i.Name_SpecifiedLanguage);
        check(items["sx"] == "新版声匣", "installed translation for an existing ID replaces the embedded name");
        check(items["external-item"] == "新物件" && !items.ContainsKey("qzx_01"), "installed complete catalog adds new IDs and removes omitted legacy IDs");
        check(items.ContainsKey("approved-item") && !items.ContainsKey("blocked-item") && !items.ContainsKey("sx_qq"),
            "resource catalog keeps scene validation gate and does not resurrect removed embedded IDs");
        strings.LoadString("en-US");
        check(strings.GetItemID("Updated Sonance") == "sx" && strings.GetItemID("New item") == "external-item", "language reload uses installed translations with stable item IDs");

        await using var core = new CoreHostService(root, new RuntimeConfigurationStore(Path.Combine(root, "runtime.json")),
            new LocalItemFilter(Path.Combine(root, "filters.json"), Path.Combine(root, "legacy.json")), resourceSnapshots: session);
        var apply = typeof(CoreHostService).GetMethod("ApplyStatus", BindingFlags.Instance | BindingFlags.NonPublic)!;
        var reported = typeof(CoreHostService).GetField("resourceHealthReported", BindingFlags.Instance | BindingFlags.NonPublic)!;
        void Status(bool ready, string id, string state) => apply.Invoke(core, [new CoreRuntimeStatus { ResourcesReady = ready, ResourceSnapshotId = id, CoreState = state }]);
        Status(false, installed.SnapshotId, "starting");
        check(session.HasPending && !(bool)reported.GetValue(core)!, "loading CoreHost status cannot commit a pending resource snapshot");
        Status(true, "different-snapshot", "stopped");
        check(session.HasPending && !(bool)reported.GetValue(core)! && core.Status.CoreState == "faulted", "mismatched ready CoreHost status faults the session without committing resources");
        Status(true, installed.SnapshotId, "faulted");
        check(session.HasPending && !(bool)reported.GetValue(core)!, "faulted CoreHost status cannot mark a matching resource snapshot healthy");
        var confirmed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        core.PropertyChanged += (_, e) => { if (e.PropertyName == "ResourceActivation") confirmed.TrySetResult(); };
        Status(true, installed.SnapshotId, "stopped");
        await confirmed.Task.WaitAsync(TimeSpan.FromSeconds(5));
        check(!session.HasPending && session.CanRollback, "matching ready CoreHost status commits resources and exposes the previous snapshot for rollback");
    }
}
