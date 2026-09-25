using IMao_WinUI.Models;
using IMao_WinUI.Services;

/// <summary>
/// 「跳过键」的配置：默认 G，可改可禁用，与其它启用键冲突时整份拒绝且不落盘。
/// </summary>
internal static class GuideSkipKeyConfigurationTests
{
    public static void Run(string root, Action<bool, string> check)
    {
        string directory = Path.Combine(root, "guide-skip-key");
        Directory.CreateDirectory(directory);
        string path = Path.Combine(directory, "runtime.json");
        // 老配置里没有这个字段：必须得到默认的 G，而不是 0（禁用）或别的键。
        File.WriteAllText(path, "{\"MapEnabled\":false}");
        var store = new RuntimeConfigurationStore(path);
        check(store.Read().GuideSkipKey == 71 && RuntimeConfiguration.HotkeyName(store.Read().GuideSkipKey) == "G",
            "an older configuration gains the G skip key instead of a disabled one");
        check(store.Read().ToPayload()["guideSkipKey"] is 71,
            "the skip key travels to the core in the canonical configuration payload");

        var changed = store.Update(old => old with { GuideSkipKey = 72 });
        check(new RuntimeConfigurationStore(path).Read().GuideSkipKey == 72 && changed.GuideSkipKey == 72,
            "a customized skip key persists across configuration reload");
        check(new RuntimeConfigurationStore(path).Read().ToPayload()["guideSkipKey"] is 72,
            "the customized skip key is what the core receives");

        string bytes = File.ReadAllText(path);
        foreach (int collision in new[] { 90, 81, 119, 33, 34, 120 })
        {
            bool rejected = false;
            try { store.Update(old => old with { GuideSkipKey = collision, MapEnabled = true }); }
            catch (ArgumentException) { rejected = true; }
            check(rejected && store.Read() == changed && File.ReadAllText(path) == bytes,
                $"a skip key matching an enabled shortcut {collision} rejects the whole update");
        }

        foreach (int reserved in new[] { 27, 77, 121 })
        {
            bool rejected = false;
            try { store.Update(old => old with { GuideSkipKey = reserved }); }
            catch (ArgumentException) { rejected = true; }
            check(rejected && store.Read() == changed, $"a reserved key {reserved} cannot become the skip key");
        }

        var disabled = store.Update(old => old with { GuideSkipKey = 0 });
        check(disabled.GuideSkipKey == 0 && new RuntimeConfigurationStore(path).Read().GuideSkipKey == 0,
            "the skip key can be disabled and stays disabled after reload");
        check(store.Update(old => new RuntimeConfiguration()).GuideSkipKey == 71,
            "restoring defaults brings the skip key back to G");
    }
}