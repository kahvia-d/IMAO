using IMao_WinUI.Services;
using System.Text;
using System.Text.Json;

internal static class LocalMarkerProfileTests
{
    public static void Run(string root, Action<bool, string> check)
    {
        string directory = Path.Combine(root, "local-marker-profile-compatibility");
        Directory.CreateDirectory(directory);
        string path = Path.Combine(directory, "kuromap-accounts.json");
        var missing = new LocalMarkerProfileSelection(path);
        check(missing.ProfileId == "local" && missing.Warning.Length == 0 && !File.Exists(path),
            "missing legacy metadata defaults to local without creating any file");
        var missingParent = new LocalMarkerProfileSelection(Path.Combine(directory, "not-created", "kuromap-accounts.json"));
        check(missingParent.ProfileId == "local" && missingParent.Warning.Length == 0 && !Directory.Exists(Path.Combine(directory, "not-created")),
            "fresh installation does not create a configuration directory");

        string profiles = Path.Combine(directory, "SavedPoints", "profiles");
        Directory.CreateDirectory(profiles);
        var documents = new Dictionary<string, byte[]>
        {
            ["local.json"] = Encoding.UTF8.GetBytes("{\"profileId\":\"local\",\"points\":{\"local-only\":true}}"),
            ["kuro_123.json"] = Encoding.UTF8.GetBytes("{\"profileId\":\"kuro_123\",\"points\":{\"selected-only\":true}}"),
            ["kuro_999.json"] = Encoding.UTF8.GetBytes("{\"profileId\":\"kuro_999\",\"points\":{\"other-only\":true}}")
        };
        foreach (var item in documents) File.WriteAllBytes(Path.Combine(profiles, item.Key), item.Value);
        byte[] metadata = Encoding.UTF8.GetBytes("{\n  \"ActiveProfile\": \"kuro_123\",\n  \"Accounts\": {\"ignored\": true},\n  \"AutomaticSync\": true,\n  \"ConnectionMode\": 999\n}\n");
        File.WriteAllBytes(path, metadata);
        var selected = new LocalMarkerProfileSelection(path);
        check(selected.ProfileId == "kuro_123" && selected.Warning.Length == 0,
            "only explicit legacy ActiveProfile selects progress independently of account objects and connection mode");
        check(File.ReadAllBytes(path).SequenceEqual(metadata) && documents.All(item => File.ReadAllBytes(Path.Combine(profiles, item.Key)).SequenceEqual(item.Value)),
            "selection recovery leaves metadata and all independent progress files byte-for-byte unchanged");
        check(Directory.GetFiles(directory, "*", SearchOption.AllDirectories).Length == documents.Count + 1,
            "selection recovery creates no local replacement or migration file");
        byte[] withBom = new byte[] { 0xef, 0xbb, 0xbf }.Concat(metadata).ToArray();
        File.WriteAllBytes(path, withBom);
        var bomSelection = new LocalMarkerProfileSelection(path);
        check(bomSelection.ProfileId == "kuro_123" && bomSelection.Warning.Length == 0 && File.ReadAllBytes(path).SequenceEqual(withBom),
            "legacy UTF-8 BOM remains supported without rewriting its encoding");

        foreach (string id in new[] { "local", "kuro_0", "kuro_000123", "kuro_" + new string('9', 24), "kuro_456" })
        {
            Write(new { ActiveProfile = id });
            var result = new LocalMarkerProfileSelection(path);
            check(result.ProfileId == id && result.Warning.Length == 0, "valid explicit profile is preserved without guessing or merging another profile");
        }
        check(!File.Exists(Path.Combine(profiles, "kuro_456.json")), "a selected but not-yet-written profile is not created by compatibility read");

        foreach (string id in new[] { "", " ", "LOCAL", "kuro_", "kuro_1/../../local", "../local", "kuro_1\\other",
            "kuro_１２３", "kuro_١٢٣", "kuro_-1", "kuro_12a", "kuro_" + new string('9', 25), "kuro_123 ", "kuro_123\n", new string('x', 4000) })
        {
            Write(new { ActiveProfile = id });
            ExpectWarning("invalid, traversal, non-ASCII, or overlong selected profile safely falls back");
        }
        foreach (string json in new[] { "", "null", "[]", "true", "1", "\"kuro_123\"", "{}",
            "{\"Accounts\":[]}", "{\"ActiveProfile\":null}", "{\"ActiveProfile\":123}", "{\"ActiveProfile\":[]}",
            "{\"ActiveProfile\":{}}", "{\"ActiveProfile\":\"kuro_123\",\"ActiveProfile\":\"local\"}",
            "{\"activeProfile\":\"kuro_123\"}", "{\"ActiveProfile\":\"kuro_123\",}", "{\"ActiveProfile\":\"kuro_123\"} trailing" })
        {
            File.WriteAllText(path, json);
            ExpectWarning("missing, malformed, duplicate, or mistyped metadata has a safe warning");
        }
        File.WriteAllBytes(path, [0xff, 0xfe, 0xfd]);
        ExpectWarning("invalid UTF-8 metadata does not escape as a startup exception");
        File.WriteAllText(path, "{\"ActiveProfile\":\"kuro_123\",\"ignored\":" + new string('[', LocalMarkerProfileSelection.MaximumDepth + 1) + "0" +
            new string(']', LocalMarkerProfileSelection.MaximumDepth + 1) + "}");
        ExpectWarning("nested legacy metadata is bounded even for ignored fields");
        File.WriteAllText(path, "{\"ActiveProfile\":\"kuro_123\",\"ignored\":\"" + new string('x', LocalMarkerProfileSelection.MaximumBytes) + "\"}");
        ExpectWarning("oversized legacy metadata is rejected without altering the file");
        foreach (string invalidPath in new[] { "", " ", "bad\0path", directory })
        {
            var result = new LocalMarkerProfileSelection(invalidPath);
            check(result.ProfileId == "local" && result.Warning.Length > 0 &&
                (string.IsNullOrWhiteSpace(invalidPath) || !result.Warning.Contains(invalidPath, StringComparison.Ordinal)),
                "invalid path or directory instead of file produces a safe local fallback");
        }
        Write(new { ActiveProfile = "kuro_123" });
        using (var locked = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.None))
        {
            var result = new LocalMarkerProfileSelection(path);
            check(result.ProfileId == "local" && result.Warning.Length > 0, "temporarily unreadable metadata has a safe warning");
        }
        check(documents.All(item => File.ReadAllBytes(Path.Combine(profiles, item.Key)).SequenceEqual(item.Value)),
            "all malformed input cases preserve every progress profile unchanged");

        void Write(object value) => File.WriteAllText(path, JsonSerializer.Serialize(value));
        void ExpectWarning(string name)
        {
            byte[] before = File.ReadAllBytes(path);
            var result = new LocalMarkerProfileSelection(path);
            check(result.ProfileId == "local" && result.Warning.Length > 0 && File.ReadAllBytes(path).SequenceEqual(before), name);
        }
    }
}
