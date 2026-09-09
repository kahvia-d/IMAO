using IMao_WinUI.Helpers;
using IMao_WinUI.Services;
using System.Collections.Concurrent;
using System.Text.Json;

internal static class MarkerIpcTests
{
    private const string FirstPoint = "1409977912641277952";
    private const string SecondPoint = "1409980210964680704";
    private const string UnknownPoint = "1999999999999999999";
    // Retain the historical name to verify that existing progress profiles still work locally.
    private const string Profile = "kuro_910000000001";

    public static async Task RunAsync(string hostDirectory, string testRoot, Action<bool, string> check)
    {
        string directory = Path.Combine(testRoot, "marker-ipc-" + Guid.NewGuid().ToString("N"));
        string appData = Path.Combine(directory, "local-app-data");
        string savedPoints = Path.Combine(appData, "IMao-WinUI", "SavedPoints");
        Directory.CreateDirectory(savedPoints);
        // Seed the isolated legacy file before the host starts, so it cannot copy real legacy progress
        // from the executable's working directory. These IDs identify public map objects, not users.
        string legacyPath = Path.Combine(savedPoints, "account_1.json");
        string legacyBytes = JsonSerializer.Serialize(new Dictionary<string, object>
        { ["World"] = new Dictionary<string, object> { ["sx_qq"] = new[] { new { id = FirstPoint } } } });
        File.WriteAllText(legacyPath, legacyBytes);
        string filterPath = Path.Combine(directory, "filters.json");
        string filterLegacy = Path.Combine(directory, "legacy-filters.json");
        File.WriteAllText(filterLegacy, "{\"Status\":{}}");
        string profileSelectionPath = Path.Combine(directory, "kuromap-accounts.json");
        File.WriteAllText(profileSelectionPath, "{\"ActiveProfile\":\"local\"}");
        string? previousAppData = Environment.GetEnvironmentVariable("LOCALAPPDATA");
        Environment.SetEnvironmentVariable("LOCALAPPDATA", appData);
        try
        {
            await using (var core = NewCore())
            {
                var events = new ConcurrentQueue<JsonElement>();
                const ulong guideGeneration = 72057594037927937UL;
                const long guideHwnd = 1234;
                var guideCompletion = new TaskCompletionSource<JsonElement>(TaskCreationOptions.RunContinuationsAsynchronously);
                core.MarkerEvent += (_, data) =>
                {
                    events.Enqueue(data.Clone());
                    if (Text(data, "type") == "markerCompletionChanged" &&
                        data.TryGetProperty("guideSelectionGeneration", out var generation) && generation.GetUInt64() == guideGeneration)
                        guideCompletion.TrySetResult(data.Clone());
                };
                await core.EnsureStartedAsync();
                if (!core.IsConnected) throw new IOException("Marker IPC host startup failed: " + core.LastFault);
                check(core.IsConnected, "marker IPC connects a real host under isolated application data");

                var local = await Call(core, "markerGetSnapshot", new { profileId = "local" });
                check(Text(local, "profileId") == "local" && Text(local, "activeProfileId") == "local" &&
                    Point(local, FirstPoint).GetProperty("completed").GetBoolean(),
                    "real host migrates legacy completion into the independent local profile");
                check(File.ReadAllText(legacyPath) == legacyBytes, "marker migration preserves the original legacy file");
                var nearbyWithoutPosition = await Call(core, "markerGetNearbyGuide", new { profileId = "local" });
                check(!nearbyWithoutPosition.TryGetProperty("candidates", out _),
                    "F8 nearby lookup without a trusted game position cannot fabricate candidates");
                check(!events.Any(e => Text(e, "type") == "markerCandidates"),
                    "correlated F8 lookup does not publish a late unsolicited chooser");
                await VerifyGamepadWithoutMapAsync(core, local, check);

                await Call(core, "markerSelectProfile", new { profileId = Profile });
                var empty = await Call(core, "markerGetSnapshot", new { profileId = Profile });
                check(Text(empty, "activeProfileId") == Profile && empty.GetProperty("total").GetInt32() == 0,
                    "a separate progress profile starts independently of local completion");
                local = await Call(core, "markerGetSnapshot", new { profileId = "local" });
                check(Text(local, "activeProfileId") == Profile && Point(local, FirstPoint).GetProperty("completed").GetBoolean(),
                    "reading another profile does not switch the active progress profile");

                var first = await Call(core, "markerSetCompletion", new
                { profileId = Profile, stateId = 8, pointId = FirstPoint, sceneName = "wrong-scene", nameId = "wrong-type", completed = true,
                    guideSelectionGeneration = guideGeneration, guideWindowHwnd = guideHwnd });
                ulong firstRevision = Revision(first.GetProperty("point"));
                check(Text(first.GetProperty("point"), "pointId") == FirstPoint && Text(first.GetProperty("point"), "nameId") == "sx_qq" &&
                    Text(first.GetProperty("point"), "sceneName") == "World" && first.GetProperty("point").GetProperty("completed").GetBoolean(),
                    "real host resolves original point identity and atomically saves local completion");
                var guideEvent = await guideCompletion.Task.WaitAsync(TimeSpan.FromSeconds(5));
                var guidePoint = guideEvent.GetProperty("point");
                check(guideEvent.GetProperty("guideSelectionGeneration").GetUInt64() == guideGeneration &&
                    guideEvent.GetProperty("guideWindowHwnd").GetInt64() == guideHwnd && Text(guideEvent, "profileId") == Profile &&
                    Text(guidePoint, "pointId") == FirstPoint && guidePoint.GetProperty("stateId").GetInt32() == 8 &&
                    Text(guidePoint, "sceneName") == "World" && Text(guidePoint, "nameId") == "sx_qq" &&
                    guidePoint.GetProperty("completed").GetBoolean(),
                    "confirmed guide completion event preserves exact presentation generation, HWND and canonical point identity");
                await VerifyGuideProtocolRejectionsAsync(core, savedPoints, check);
                var repeated = await Call(core, "markerSetCompletion", new
                { profileId = Profile, stateId = 8, pointId = FirstPoint, completed = true });
                check(Revision(repeated.GetProperty("point")) == firstRevision,
                    "repeating a completion does not create another point revision");
                await Call(core, "markerSetCompletion", new { profileId = Profile, stateId = 8, pointId = SecondPoint, completed = true });
                var reversed = await Call(core, "markerSetCompletion", new { profileId = Profile, stateId = 8, pointId = FirstPoint, completed = false });
                ulong reversedRevision = Revision(reversed.GetProperty("point"));
                var page = await Call(core, "markerGetSnapshot", new { profileId = Profile, offset = 0, limit = 1 });
                var nextPage = await Call(core, "markerGetSnapshot", new { profileId = Profile, offset = 1, limit = 1 });
                var points = page.GetProperty("points").EnumerateArray().Concat(nextPage.GetProperty("points").EnumerateArray()).ToArray();
                check(page.GetProperty("total").GetInt32() == 2 && page.GetProperty("hasMore").GetBoolean() &&
                    !nextPage.GetProperty("hasMore").GetBoolean() && points.Select(p => Text(p, "pointId")).Distinct().Count() == 2 &&
                    !points.Single(p => Text(p, "pointId") == FirstPoint).GetProperty("completed").GetBoolean() &&
                    reversedRevision > firstRevision,
                    "local completion undo creates a new revision and remains visible across snapshot pages");

                check(await RejectedAsync(() => Call(core, "markerSetCompletion", new
                { profileId = Profile, stateId = 8, pointId = UnknownPoint, completed = true }), "unknown-public-point"),
                    "native IPC refuses locally invented point IDs");

                await Call(core, "markerSelectProfile", new { profileId = "local" });
                check(await RejectedAsync(() => Call(core, "markerSetCompletion", new
                { profileId = Profile, stateId = 8, pointId = FirstPoint, completed = true }), "profile-mismatch"),
                    "a delayed action from the previous progress profile cannot modify the active one");
                local = await Call(core, "markerGetSnapshot", new { profileId = "local" });
                check(Point(local, FirstPoint).GetProperty("completed").GetBoolean() && local.GetProperty("total").GetInt32() == 1,
                    "edits to another profile leave migrated local progress unchanged");
                await Call(core, "markerSelectProfile", new { profileId = Profile });
                var beforeRestart = await Call(core, "markerGetSnapshot", new { profileId = Profile });
                string profilePath = Path.Combine(savedPoints, "profiles", Profile + ".json");
                check(File.Exists(profilePath), "marker profile is committed inside the isolated application data directory");
                await core.RestartAsync();
                var afterRestart = await Call(core, "markerGetSnapshot", new { profileId = Profile });
                check(beforeRestart.GetRawText() == afterRestart.GetRawText() && Text(afterRestart, "activeProfileId") == Profile,
                    "restarting the real CoreHost restores the selected profile, progress and revisions");
                check(Point(afterRestart, SecondPoint).GetProperty("completed").GetBoolean() &&
                    !Point(afterRestart, FirstPoint).GetProperty("completed").GetBoolean(),
                    "local completion and undo both survive a real host restart");
                check(events.Any(e => Text(e, "type") == "markerCompletionChanged" && Text(e, "profileId") == Profile) &&
                    events.Any(e => Text(e, "type") == "markerProfileChanged" && Text(e, "profileId") == Profile),
                    "real marker IPC delivers profile-scoped completion and selection events");
                await core.ShutdownAsync();
            }

            // A new managed service has no in-memory marker document; only persisted state can satisfy this check.
            File.WriteAllText(profileSelectionPath, JsonSerializer.Serialize(new
            { ActiveProfile = Profile, AutomaticSync = true, Accounts = new[] { new { UserId = Profile[5..], DisplayName = "历史测试档案" } } }));
            await using (var reopened = NewCore())
            {
                var restored = await Call(reopened, "markerGetSnapshot", new { profileId = Profile });
                check(Text(restored, "activeProfileId") == Profile && Point(restored, SecondPoint).GetProperty("completed").GetBoolean() &&
                    !Point(restored, FirstPoint).GetProperty("completed").GetBoolean(),
                    "a fresh local-only client restores the previously selected historical profile and its durable progress");
                await reopened.ShutdownAsync();
            }
        }
        finally { Environment.SetEnvironmentVariable("LOCALAPPDATA", previousAppData); }

        CoreHostService NewCore() => new(Path.GetFullPath(hostDirectory),
            new RuntimeConfigurationStore(Path.Combine(directory, "runtime.json")), new LocalItemFilter(filterPath, filterLegacy),
            new LocalMarkerProfileSelection(profileSelectionPath));
    }

    private static async Task VerifyGamepadWithoutMapAsync(CoreHostService core, JsonElement originalSnapshot, Action<bool, string> check)
    {
        // This host has only completed its handshake. Never start runtime capture or interact with a game.
        var context = await Call(core, "markerGetGamepadContext", new { });
        check(!context.GetProperty("available").GetBoolean() && !context.GetProperty("bigMap").GetBoolean() &&
            !context.GetProperty("gameplay").GetBoolean() && !context.GetProperty("nearbyAvailable").GetBoolean() &&
            !context.GetProperty("gameFocused").GetBoolean() && context.GetProperty("gameHwnd").GetUInt64() == 0 &&
            Text(context, "profileId") == "local" && Text(context, "sceneName").Length == 0,
            "idle real host provides unavailable gamepad context without a game window or invented player position");
        ulong generation = context.GetProperty("contextGeneration").GetUInt64();
        check(generation > 0 && await RejectedAsync(() => Call(core, "markerGetGamepadTargets", new
            { profileId = "local", contextGeneration = generation })) && core.IsConnected,
            "real host refuses gamepad targets when no map is observed even with its current context generation");
        foreach (string status in new[] { "returning", "failed" })
        {
            var display = await Call(core, "markerRouteGamepadReturnStatus", new { sessionId = 999999UL, status });
            check(!display.GetProperty("visible").GetBoolean(),
                "return display cannot invent a host lease or input session from an unknown session ID");
        }
        check(await RejectedAsync(() => Call(core, "markerRouteGamepadReturnStatus", new
            { sessionId = 999999UL, status = "toolbar" })) && core.IsConnected,
            "return display protocol cannot upgrade a stopped session into toolbar input");
        var after = await Call(core, "markerGetSnapshot", new { profileId = "local" });
        check(after.GetRawText() == originalSnapshot.GetRawText(),
            "gamepad context reads and rejected target requests preserve point completion and profile revisions");
    }

    private static async Task VerifyGuideProtocolRejectionsAsync(CoreHostService core, string savedPoints, Action<bool, string> check)
    {
        string profilePath = Path.Combine(savedPoints, "profiles", Profile + ".json");
        string beforeBytes = await File.ReadAllTextAsync(profilePath);
        string beforeSnapshot = (await Call(core, "markerGetSnapshot", new { profileId = Profile })).GetRawText();
        foreach (string field in new[] { "guideSelectionGeneration", "guideWindowHwnd" })
        {
            foreach (object? invalid in new object?[] { 0, -1, 1.5, true, "1", null })
            {
                var command = new Dictionary<string, object?>
                {
                    ["profileId"] = Profile, ["stateId"] = 8, ["pointId"] = FirstPoint, ["completed"] = false,
                    ["guideSelectionGeneration"] = 73, ["guideWindowHwnd"] = 1234, [field] = invalid
                };
                bool rejected = await RejectedAsync(() => Call(core, "markerSetCompletion", command));
                var snapshot = await Call(core, "markerGetSnapshot", new { profileId = Profile });
                check(rejected && core.IsConnected && snapshot.GetRawText() == beforeSnapshot &&
                    await File.ReadAllTextAsync(profilePath) == beforeBytes,
                    "invalid guide completion context cannot change canonical completion, revision or persisted bytes");
            }
        }
        foreach (var command in new Dictionary<string, object?>[]
        {
            new() { ["hwnd"] = 1234, ["profileId"] = Profile },
            new() { ["hwnd"] = 1234, ["profileId"] = Profile, ["stateId"] = 8, ["pointId"] = FirstPoint },
            new() { ["hwnd"] = ulong.MaxValue },
            new() { ["hwnd"] = -1 },
            new() { ["hwnd"] = true },
            new() { ["hwnd"] = 0, ["profileId"] = Profile, ["stateId"] = 8, ["pointId"] = FirstPoint, ["selectionGeneration"] = 1 },
            new() { ["hwnd"] = 1234, ["profileId"] = "local", ["stateId"] = 8, ["pointId"] = FirstPoint, ["selectionGeneration"] = 1 }
        })
            check(await RejectedAsync(() => Call(core, "markerSetGuideWindow", command)) && core.IsConnected,
                "native guide registration rejects incomplete identities, stale profiles and invalid HWNDs");
        var unregistered = await Call(core, "markerSetGuideWindow", new { hwnd = 0 });
        check(unregistered.ValueKind == JsonValueKind.Object && core.IsConnected &&
            await File.ReadAllTextAsync(profilePath) == beforeBytes,
            "zero HWND safely unregisters a guide without touching a real window or changing point progress");
    }

    private static Task<JsonElement> Call(CoreHostService core, string type, object arguments) => core.ExecuteMarkerAsync(type, arguments);
    private static JsonElement Point(JsonElement snapshot, string pointId) => snapshot.GetProperty("points").EnumerateArray()
        .Single(point => Text(point, "pointId") == pointId);
    private static string Text(JsonElement value, string key) => value.TryGetProperty(key, out var field) && field.ValueKind == JsonValueKind.String
        ? field.GetString() ?? string.Empty : string.Empty;
    private static ulong Revision(JsonElement value) => value.GetProperty("revision").GetUInt64();
    private static async Task<bool> RejectedAsync(Func<Task<JsonElement>> action, string? expected = null)
    {
        try { await action(); return false; }
        catch (InvalidOperationException exception) { return expected is null || exception.Message.Contains(expected, StringComparison.Ordinal); }
    }
}
