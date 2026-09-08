using IMao_WinUI.Helpers;
using IMao_WinUI.Models;
using IMao_WinUI.Services;
using System.Diagnostics;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;

internal static class RoutePlanningTests
{
    public static void Run(Action<bool, string> check)
    {
        using var data = JsonDocument.Parse("""
            {"revision":9,"profileId":"local","enabled":true,"selectedCount":2,"hiddenCount":1,
             "sceneId":1,"tool":"lasso","navigationStatus":"waitingForLocation",
             "start":{"valid":true,"x":0,"y":0,"source":"manual","generation":12},
             "selected":[{"key":"8:1409977912641277952","stateId":8,"pointId":"1409977912641277952","x":10,"y":20},
                         {"key":"8:1409980210964680704","stateId":8,"pointId":"1409980210964680704","x":10,"y":20}],
             "preview":{"id":"preview","planarLength":42.5,"stops":[{"pointId":"1409980210964680704","order":1}]},
             "active":{"id":"active","stops":[{"pointId":"1409977912641277952","order":1,"skipped":true}]},
             "savedRoutes":[{"id":"saved-one","name":"一号路线","sceneName":"World"}]}
            """);
        var state = RoutePlanningState.FromJson(data.RootElement);
        check(state.Selected.Length == 2 && state.Selected[0].PointId == "1409977912641277952" &&
            state.Selected[1].PointId != state.Selected[0].PointId && state.Selected[1].X == state.Selected[0].X,
            "automatic route snapshots preserve opaque large IDs and distinct targets at equal coordinates");
        check(state.Preview?.Id == "preview" && state.Active?.Id == "active" && state.Active.Stops[0].Skipped &&
            state.Preview.PlanarLength == 42.5, "automatic route preview and active navigation remain independent");
        check(state.Start.Valid && state.Start.X == 0 && state.Start.Generation == 12 && state.HiddenCount == 1 &&
            state.Selected[0].FloorLabel == "未知", "route UI preserves origin starts and reports unknown floors explicitly");
        check(state.NavigationLabel.Contains("定位") && state.SavedRoutes[0].Label.Contains("一号路线"),
            "route status and saved-route names have readable UI labels");
        VerifyHotkeyConfiguration(check);
    }

    private static void VerifyHotkeyConfiguration(Action<bool, string> check)
    {
        string directory = Path.Combine(Path.GetTempPath(), "imao-route-hotkeys-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            string path = Path.Combine(directory, "runtime.json");
            File.WriteAllText(path, "{\"StatusBarEnabled\":false,\"MapUpdateCycle\":95}");
            var store = new RuntimeConfigurationStore(path);
            check(store.Read() is { NearestCompletionKey: 90, ManualRouteKey: 81, CurrentTargetGuideKey: 119,
                GuidePreviousImageKey: 33, GuideNextImageKey: 34,
                StatusBarEnabled: false, MapUpdateCycle: 95 } && store.LoadError.Length == 0,
                "legacy configuration supplies Z/Q/F8/PageUp/PageDown without resetting existing preferences");
            var payload = store.Read().ToPayload();
            check(payload["guidePreviousImageKey"] is 33 && payload["guideNextImageKey"] is 34 &&
                RuntimeConfiguration.HotkeyName(33) == "PageUp" && RuntimeConfiguration.HotkeyName(34) == "PageDown",
                "guide paging uses named PageUp/PageDown choices and the canonical configuration protocol fields");
            string migrationPath = Path.Combine(directory, "customized-legacy.json");
            File.WriteAllText(migrationPath, "{\"NearestCompletionKey\":65,\"ManualRouteKey\":66,\"CurrentTargetGuideKey\":118,\"StatusBarEnabled\":false}");
            var migrated = new RuntimeConfigurationStore(migrationPath);
            check(migrated.Read() is { NearestCompletionKey: 65, ManualRouteKey: 66, CurrentTargetGuideKey: 118,
                GuidePreviousImageKey: 33, GuideNextImageKey: 34, StatusBarEnabled: false } && migrated.LoadError.Length == 0,
                "existing customized three-key configuration gains paging defaults without replacing old bindings");
            var changed = store.Update(old => old with { NearestCompletionKey = 0, ManualRouteKey = 0, CurrentTargetGuideKey = 118 });
            check(new RuntimeConfigurationStore(path).Read() == changed,
                "multiple disabled hotkeys and a changed guide key persist across configuration reload");
            changed = store.Update(old => old with { NearestCompletionKey = 81, ManualRouteKey = 90 });
            string bytes = File.ReadAllText(path);
            foreach (int invalidKey in new[] { 81, 27, 77, 121, 16, 17, 18, -1, 256 })
            {
                bool rejected = false;
                try { store.Update(old => old with { CurrentTargetGuideKey = invalidKey, MapEnabled = false, StatusBarEnabled = true }); }
                catch (ArgumentException) { rejected = true; }
                check(rejected && store.Read() == changed && File.ReadAllText(path) == bytes,
                    "duplicate or reserved hotkey rejects the entire persisted update including unrelated preferences");
            }
            check(new RuntimeConfigurationStore(path).Read() == changed,
                "valid swapped bindings survive reload after rejected updates");
            changed = store.Update(old => old with { GuidePreviousImageKey = 48, GuideNextImageKey = 49 });
            check(new RuntimeConfigurationStore(path).Read() == changed,
                "both customized guide paging bindings persist without resetting the other shortcuts");
            bytes = File.ReadAllText(path);
            foreach (var invalid in new[]
            {
                changed with { GuidePreviousImageKey = 81 },
                changed with { GuidePreviousImageKey = 90 },
                changed with { GuidePreviousImageKey = 118 },
                changed with { GuidePreviousImageKey = 49 },
                changed with { GuideNextImageKey = 48 },
                changed with { CurrentTargetGuideKey = 48 },
                changed with { GuideNextImageKey = 27 },
                changed with { GuidePreviousImageKey = 77 },
                changed with { GuideNextImageKey = 121 }
            })
            {
                bool rejected = false;
                try { store.Update(_ => invalid with { MapEnabled = false }); }
                catch (ArgumentException) { rejected = true; }
                check(rejected && store.Read() == changed && File.ReadAllText(path) == bytes,
                    "paging collisions with every existing action, each other and reserved keys reject atomically");
            }
            changed = store.Update(old => old with { NearestCompletionKey = 33, ManualRouteKey = 34,
                GuidePreviousImageKey = 0, GuideNextImageKey = 0 });
            check(new RuntimeConfigurationStore(path).Read() == changed,
                "PageUp and PageDown can be assigned to other actions while both image shortcuts remain disabled");
            changed = store.Update(_ => new RuntimeConfiguration());
            check(new RuntimeConfigurationStore(path).Read() == new RuntimeConfiguration(),
                "restoring configuration defaults restores both guide paging keys and the original three bindings");
        }
        finally { Directory.Delete(directory, recursive: true); }
    }

    public static async Task RunIpcAsync(string hostDirectory, string root, Action<bool, string> check)
    {
        string directory = Path.Combine(root, "auto-route-ipc");
        Directory.CreateDirectory(directory);
        string appData = Path.Combine(directory, "app-data");
        string savedPoints = Path.Combine(appData, "IMao-WinUI", "SavedPoints");
        Directory.CreateDirectory(savedPoints);
        File.WriteAllText(Path.Combine(savedPoints, "account_1.json"), "{}");
        string? previousAppData = Environment.GetEnvironmentVariable("LOCALAPPDATA");
        Environment.SetEnvironmentVariable("LOCALAPPDATA", appData);
        try
        {
            await using var core = new CoreHostService(hostDirectory,
                new RuntimeConfigurationStore(Path.Combine(directory, "runtime.json")),
                new LocalItemFilter(Path.Combine(directory, "filters.json"), Path.Combine(directory, "legacy.json")));
            var state = await core.ExecuteRoutePlanningAsync("state");
            check(core.IsConnected && state.SelectedCount == 0 && state.Active is null,
                "automatic route state travels through real host data acknowledgements before game startup");
            check(await RejectedAsync(() => core.ExecuteRoutePlanningAsync("guide")),
                "guide without an active route is rejected instead of guessing a point");
            bool rejected = false;
            try { await core.ExecuteRoutePlanningAsync("unknown-route-action"); }
            catch (InvalidOperationException) { rejected = true; }
            check(rejected && core.IsConnected, "rejected route commands fail their matching request without faulting the core");
            var snapshots = await Task.WhenAll(Enumerable.Range(0, 5).Select(_ => core.ExecuteRoutePlanningAsync("state")));
            check(snapshots.All(snapshot => snapshot.ProfileId == state.ProfileId),
                "concurrent route requests correlate to the active session");
            await VerifyAuthoritativeGuideQueryAsync(core, appData, savedPoints, check);

            const string firstKey = "8:1409977912641277952";
            const string secondKey = "8:1409980210964680704";
            await core.ExecuteRoutePlanningAsync("new", new { sceneId = 1 });
            state = await core.ExecuteRoutePlanningAsync("setStart", new { sceneId = 1, x = 0, y = 0 });
            ulong beforeSelectionGeneration = state.Generation;
            state = await core.ExecuteRoutePlanningAsync("add", new { keys = new[] { firstKey, secondKey } });
            check(state.SelectedCount == 2 && state.Start.Valid && state.Start.X == 0,
                "real host accepts original marker identities and a manually chosen origin");
            ulong selectionGeneration = state.Generation;
            foreach (string action in new[] { "add", "toggle", "setStart" })
            {
                var payload = new Dictionary<string, object?>
                {
                    ["keys"] = new[] { firstKey }, ["key"] = firstKey,
                    ["sceneId"] = 1, ["x"] = 123, ["y"] = 456,
                    ["expectedSceneId"] = 2, ["expectedGeneration"] = selectionGeneration
                };
                bool staleScene = await RejectedAsync(() => core.ExecuteRoutePlanningAsync(action, payload), "地图");
                payload["expectedSceneId"] = 1;
                payload["expectedGeneration"] = beforeSelectionGeneration;
                bool staleGeneration = await RejectedAsync(() => core.ExecuteRoutePlanningAsync(action, payload), "草稿");
                state = await core.ExecuteRoutePlanningAsync("state");
                check(staleScene && staleGeneration && state.Generation == selectionGeneration &&
                    state.Selected.Select(stop => stop.Key).SequenceEqual(new[] { firstKey, secondKey }) && state.Start is { X: 0, Y: 0 },
                    $"stale rendered scene or draft generation rejects {action} without changing selected targets or the origin");
            }
            using (var publicData = JsonDocument.Parse(await File.ReadAllTextAsync(
                Path.Combine(hostDirectory, "Assets", "KuroMap", "states", "state-8.json"))))
            {
                string[] otherKeys = publicData.RootElement.EnumerateArray()
                    .SelectMany(category => category.GetProperty("location").EnumerateArray())
                    .Select(point => "8:" + point.GetProperty("id").GetString())
                    .Where(key => key != firstKey && key != secondKey).Distinct(StringComparer.Ordinal)
                    .OrderBy(key => key, StringComparer.Ordinal).Take(499).ToArray();
                check(otherKeys.Length == 499, "capacity regression uses 499 additional original public marker IDs");
                bool overLimit = await RejectedAsync(() => core.ExecuteRoutePlanningAsync("add", new { keys = otherKeys }), "500");
                state = await core.ExecuteRoutePlanningAsync("state");
                check(overLimit && state.Selected.Select(stop => stop.Key).SequenceEqual(new[] { firstKey, secondKey }),
                    "bulk append reaching 501 targets rejects the entire gesture and preserves the original selected set");
                state = await core.ExecuteRoutePlanningAsync("add", new { keys = otherKeys.Take(498).ToArray() });
                var fullSet = state.Selected.Select(stop => stop.Key).ToArray();
                check(state.SelectedCount == 500 && fullSet.Distinct(StringComparer.Ordinal).Count() == 500,
                    "the full 500-target capacity remains selectable with no duplicate identities");
                overLimit = await RejectedAsync(() => core.ExecuteRoutePlanningAsync("add", new { keys = new[] { otherKeys[498] } }), "500");
                state = await core.ExecuteRoutePlanningAsync("state");
                check(overLimit && state.Selected.Select(stop => stop.Key).SequenceEqual(fullSet),
                    "adding a 501st target at capacity changes neither selection membership nor order");
                state = await core.ExecuteRoutePlanningAsync("undo");
                check(state.Selected.Select(stop => stop.Key).SequenceEqual(new[] { firstKey, secondKey }),
                    "rejected over-capacity gestures do not consume selection undo history");
            }
            state = await GenerateAsync(core);
            check(state.Preview?.Stops.Length == 2 && state.Active is null && !state.Computing,
                "background route generation arrives through routePlanningChanged before activation");
            state = await core.ExecuteRoutePlanningAsync("activate");
            string activeId = state.Active!.Id;
            string currentKey = state.CurrentTarget!.Key;
            check(state.NavigationStatus == "waitingForLocation" && state.Active.Stops.Length == 2,
                "route activation persists but dynamic guidance waits for a valid game location");
            check(state.Preview is null, "activating a preview consumes it so the active route has no stale editable alias");
            state = await core.ExecuteRoutePlanningAsync("skip", new { key = currentKey, routeId = activeId, profileId = "local" });
            string activePath = Path.Combine(appData, "IMao-WinUI", "SavedRoutes", "Auto", "local", activeId + ".json");
            string skippedRouteBytes = await File.ReadAllTextAsync(activePath);
            bool consumedPreview = await RejectedAsync(() => core.ExecuteRoutePlanningAsync("save", new { target = "preview" }), "预览");
            state = await core.ExecuteRoutePlanningAsync("state");
            check(consumedPreview && state.Preview is null && state.Active!.Stops.Single(stop => stop.Key == currentKey).Skipped &&
                await File.ReadAllTextAsync(activePath) == skippedRouteBytes,
                "saving an already activated preview is rejected without overwriting the active route's durable skip progress");
            state = await core.ExecuteRoutePlanningAsync("undoSkip");
            check(state.CurrentTarget?.Key == currentKey, "undo restores the target after verifying consumed-preview save isolation");

            await core.ExecuteRoutePlanningAsync("new", new { sceneId = 1 });
            await core.ExecuteRoutePlanningAsync("setStart", new { sceneId = 1, x = 5, y = 5 });
            await core.ExecuteRoutePlanningAsync("add", new { keys = new[] { firstKey } });
            state = await GenerateAsync(core);
            check(state.Active?.Id == activeId && state.Active.Stops.Length == 2 && state.Preview?.Stops.Length == 1,
                "creating a second route preview preserves the active route and its target order");

            string savedPreviewId = state.Preview!.Id;
            await core.ExecuteRoutePlanningAsync("save", new { target = "preview", name = "预览加载隔离测试" });
            state = await core.ExecuteRoutePlanningAsync("load", new { routeId = savedPreviewId });
            check(state.Active?.Id == savedPreviewId && state.Preview is null,
                "loading a saved preview consumes its matching draft alias just like activation");
            state = await core.ExecuteRoutePlanningAsync("skip", new
                { key = state.CurrentTarget!.Key, routeId = savedPreviewId, profileId = "local" });
            string loadedPreviewPath = Path.Combine(appData, "IMao-WinUI", "SavedRoutes", "Auto", "local", savedPreviewId + ".json");
            string loadedPreviewBytes = await File.ReadAllTextAsync(loadedPreviewPath);
            bool loadedPreviewRejected = await RejectedAsync(() => core.ExecuteRoutePlanningAsync("save", new { target = "preview" }), "预览");
            state = await core.ExecuteRoutePlanningAsync("state");
            check(loadedPreviewRejected && state.Active!.Stops.Single().Skipped && state.Preview is null &&
                await File.ReadAllTextAsync(loadedPreviewPath) == loadedPreviewBytes,
                "save-preview then load then skip cannot overwrite durable progress by saving the old preview again");
            await core.ExecuteRoutePlanningAsync("load", new { routeId = activeId });
            state = await core.ExecuteRoutePlanningAsync("resume");
            check(state.Active?.Id == activeId && state.CurrentTarget?.Key == currentKey &&
                state.Active.Stops.All(stop => !stop.Skipped && !stop.Completed),
                "the saved-preview load regression leaves the original active route independently unchanged");

            string wrongTarget = currentKey == firstKey ? secondKey : firstKey;
            bool targetMismatch = await RejectedAsync(() => core.ExecuteRoutePlanningAsync("complete",
                new { key = wrongTarget, routeId = activeId, profileId = "local" }), "目标");
            bool routeMismatch = await RejectedAsync(() => core.ExecuteRoutePlanningAsync("skip",
                new { key = currentKey, routeId = "stale-route-id", profileId = "local" }), "路线");
            state = await core.ExecuteRoutePlanningAsync("state");
            check(targetMismatch && routeMismatch && state.CurrentTarget?.Key == currentKey &&
                state.Active!.Stops.All(stop => !stop.Completed && !stop.Skipped),
                "delayed completion and skip requests cannot modify a different target or active route");

            state = await core.ExecuteRoutePlanningAsync("complete", new { key = currentKey, routeId = activeId, profileId = "local" });
            check(state.Active!.Stops.Single(stop => stop.Key == currentKey).Completed && state.CurrentTarget?.Key != currentKey,
                "confirmed route completion advances only after durable marker completion");
            string nextKey = state.CurrentTarget!.Key;
            targetMismatch = await RejectedAsync(() => core.ExecuteRoutePlanningAsync("complete",
                new { key = currentKey, routeId = activeId, profileId = "local" }), "目标");
            state = await core.ExecuteRoutePlanningAsync("state");
            check(targetMismatch && state.CurrentTarget?.Key == nextKey && !state.CurrentTarget.Completed,
                "replaying the previous completion click cannot complete the newly advanced target");
            string profilePath = Path.Combine(savedPoints, "profiles", "local.json");
            bool completionFailed = false;
            using (var held = new FileStream(profilePath, FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                try { await core.ExecuteRoutePlanningAsync("complete", new { key = nextKey, routeId = activeId, profileId = "local" }); }
                catch (InvalidOperationException) { completionFailed = true; }
            }
            state = await core.ExecuteRoutePlanningAsync("state");
            check(completionFailed && state.CurrentTarget?.Key == nextKey && !state.CurrentTarget.Completed,
                "a failed marker completion commit leaves route navigation on the same target");
            state = await core.ExecuteRoutePlanningAsync("skip", new { key = nextKey, routeId = activeId, profileId = "local" });
            check(state.NavigationStatus == "finished" && state.Active!.Stops.Single(stop => stop.Key == nextKey) is { Skipped: true, Completed: false },
                "route skip finishes the route without changing the skipped marker completion");
            state = await core.ExecuteRoutePlanningAsync("undoSkip");
            check(state.CurrentTarget?.Key == nextKey && !state.CurrentTarget.Skipped,
                "undo skip restores the original remaining target");
            state = await GenerateAsync(core, "replan");
            check(state.Preview is { Start: { Valid: true, Source: "manual", X: 5, Y: 5 } } &&
                state.Preview.Stops.Select(stop => stop.Key).SequenceEqual(new[] { nextKey }) &&
                state.Active?.Id == activeId && state.CurrentTarget?.Key == nextKey && state.NavigationStatus == "waitingForLocation",
                "explicit replan without player localization uses the chosen manual origin and only remaining targets while preserving the active route");
            state = await core.ExecuteRoutePlanningAsync("save", new { name = "IPC 自动路线", target = "active" });
            check(state.SavedRoutes.Any(route => route.Id == activeId && route.Name == "IPC 自动路线"),
                "automatic route save returns the profile-scoped saved-route list");
            state = await core.ExecuteRoutePlanningAsync("load", new { routeId = activeId });
            check(state.NavigationStatus == "paused" && state.CurrentTarget?.Key == nextKey,
                "explicit route load preserves durable completion and restores paused navigation");
            await core.RestartAsync();
            var restarted = await core.ExecuteRoutePlanningAsync("state");
            check(core.IsConnected && restarted.Active?.Id == activeId && restarted.NavigationStatus == "paused" &&
                restarted.CurrentTarget?.Key == nextKey && core.RoutePlanning.Revision == restarted.Revision,
                "fresh host resets snapshot revision and restores the explicit active route paused with persisted completion");

            const string otherProfile = "route-ipc-other";
            await core.ExecuteMarkerAsync("markerSelectProfile", new { profileId = otherProfile });
            state = await core.ExecuteRoutePlanningAsync("state");
            check(state.ProfileId == otherProfile && state.Active is null && state.SelectedCount == 0 && state.SavedRoutes.Length == 0,
                "switching progress profile clears the old draft and exposes no previous profile routes");
            bool profileMismatch = await RejectedAsync(() => core.ExecuteRoutePlanningAsync("complete",
                new { key = nextKey, routeId = activeId, profileId = "local" }), "档案");
            bool missingRoute = await RejectedAsync(() => core.ExecuteRoutePlanningAsync("load", new { routeId = activeId }));
            await core.ExecuteRoutePlanningAsync("new", new { sceneId = 1, profileId = otherProfile });
            state = await core.ExecuteRoutePlanningAsync("add", new { keys = new[] { currentKey }, profileId = otherProfile });
            check(profileMismatch && missingRoute && state.SelectedCount == 1 && !state.Selected[0].Completed && state.Active is null,
                "a new profile cannot load or mutate the prior route and does not inherit its completed markers");
            await core.ExecuteMarkerAsync("markerSelectProfile", new { profileId = "local" });
            state = await core.ExecuteRoutePlanningAsync("state");
            check(state.Active?.Id == activeId && state.CurrentTarget?.Key == nextKey && state.NavigationStatus == "paused" &&
                state.Active.Stops.Single(stop => stop.Key == currentKey).Completed,
                "returning to the original profile restores its own paused route and unchanged durable completion");
            await VerifyStopDeleteAndGuideAsync(core, appData, savedPoints, activeId, currentKey, nextKey, otherProfile, check);
            await VerifyHotkeyConfigurationIpcAsync(core, Path.Combine(directory, "runtime.json"), check);
            await core.ShutdownAsync();
            await VerifyNativeHotkeyConfigurationAsync(hostDirectory, check);
        }
        finally { Environment.SetEnvironmentVariable("LOCALAPPDATA", previousAppData); }
    }

    private static async Task VerifyAuthoritativeGuideQueryAsync(CoreHostService core, string appData, string savedPoints,
        Action<bool, string> check)
    {
        const string profile = "route-guide-query";
        const string firstKey = "8:1409977912641277952";
        const string secondKey = "8:1409980210964680704";
        int selectedEvents = 0;
        void MarkerEvent(object? sender, JsonElement value)
        {
            if (value.TryGetProperty("type", out var type) && type.GetString() == "markerSelected")
                Interlocked.Increment(ref selectedEvents);
        }
        Task<JsonElement> Query(string requestedProfile = profile, string staleKey = "stale-target") =>
            core.ExecuteMarkerAsync("markerGetRouteGuide", new
            { profileId = requestedProfile, routeId = "stale-route", key = staleKey, screenX = 321.25, screenY = 654.5 });
        static bool IsTarget(JsonElement result, string key)
        {
            var selection = result.GetProperty("selection");
            return selection.ValueKind == JsonValueKind.Object &&
                selection.GetProperty("stateId").GetInt32() + ":" + selection.GetProperty("pointId").GetString() == key &&
                !selection.GetProperty("completed").GetBoolean();
        }
        static async Task<string?> Bytes(string path) => File.Exists(path) ? await File.ReadAllTextAsync(path) : null;
        core.MarkerEvent += MarkerEvent;
        try
        {
            var none = await Query("local");
            check(none.GetProperty("profileId").GetString() == "local" && none.GetProperty("routeId").GetString() == "" &&
                none.GetProperty("selection").ValueKind == JsonValueKind.Null,
                "authoritative guide query returns null without an active route instead of reusing an old target");
            await core.ExecuteMarkerAsync("markerSelectProfile", new { profileId = profile });
            await core.ExecuteRoutePlanningAsync("new", new { sceneId = 1, profileId = profile });
            await core.ExecuteRoutePlanningAsync("setStart", new { sceneId = 1, x = 0, y = 0, profileId = profile });
            await core.ExecuteRoutePlanningAsync("add", new { keys = new[] { firstKey, secondKey }, profileId = profile });
            await GenerateAsync(core);
            var before = await core.ExecuteRoutePlanningAsync("activate", new { profileId = profile });
            string routeId = before.Active!.Id;
            string current = before.CurrentTarget!.Key;
            string next = before.Active.Stops.First(stop => stop.Key != current).Key;
            string routePath = Path.Combine(appData, "IMao-WinUI", "SavedRoutes", "Auto", profile, routeId + ".json");
            string pointerPath = Path.Combine(Path.GetDirectoryName(routePath)!, "active.json");
            string progressPath = Path.Combine(savedPoints, "profiles", profile + ".json");
            var routeBytes = await Bytes(routePath);
            var pointerBytes = await Bytes(pointerPath);
            var progressBytes = await Bytes(progressPath);
            var target = await Query();
            var sameTarget = await Query();
            var selection = target.GetProperty("selection");
            var expected = before.CurrentTarget!;
            check(IsTarget(target, current) && IsTarget(sameTarget, current) && target.GetProperty("routeId").GetString() == routeId &&
                target.GetProperty("revision").GetUInt64() == before.Revision && sameTarget.GetProperty("revision").GetUInt64() == before.Revision &&
                selection.GetProperty("profileId").GetString() == profile && selection.GetProperty("sceneName").GetString() == before.Active.SceneName &&
                selection.GetProperty("nameId").GetString() == expected.NameId && selection.GetProperty("countryId").GetInt32() == expected.CountryId &&
                selection.GetProperty("floorId").GetString() == expected.FloorId && selection.GetProperty("level").GetString() == expected.Level &&
                selection.GetProperty("screenX").GetDouble() == 321.25 && selection.GetProperty("screenY").GetDouble() == 654.5,
                "guide query resolves the first unfinished target with complete canonical identity and preserves revision across repeated reads");
            var after = await core.ExecuteRoutePlanningAsync("state");
            check(JsonSerializer.Serialize(before with { Revision = 0 }) == JsonSerializer.Serialize(after with { Revision = 0 }) &&
                await Bytes(routePath) == routeBytes && await Bytes(pointerPath) == pointerBytes && await Bytes(progressPath) == progressBytes,
                "guide queries do not change navigation, route order, route files, active pointer or completion records");
            bool rejected = false;
            try { await Query("local"); }
            catch (InvalidOperationException) { rejected = true; }
            var afterRejected = await Query();
            check(rejected && IsTarget(afterRejected, current) && afterRejected.GetProperty("revision").GetUInt64() == after.Revision &&
                await Bytes(routePath) == routeBytes && await Bytes(progressPath) == progressBytes,
                "guide queries reject a different profile without selecting or modifying that profile's point");

            await core.ExecuteRoutePlanningAsync("skip", new { profileId = profile, routeId, key = current });
            check(IsTarget(await Query(), next), "authoritative guide query excludes a skipped target in the fixed route order");
            await core.ExecuteRoutePlanningAsync("undoSkip", new { profileId = profile, routeId });
            check(IsTarget(await Query(), current), "undoing a skip restores the original unfinished guide target");
            await core.ExecuteMarkerAsync("markerSetCompletion", new
            { profileId = profile, stateId = 8, pointId = current.Split(':')[1], completed = true,
                guideSelectionGeneration = 701, guideWindowHwnd = 1234 });
            check(IsTarget(await Query(staleKey: current), next),
                "immediately after guide completion acknowledgement a stale F8 key resolves to the next unfinished target");
            await core.ExecuteMarkerAsync("markerSetCompletion", new
            { profileId = profile, stateId = 8, pointId = next.Split(':')[1], completed = true });
            var finished = await Query(staleKey: next);
            check(finished.GetProperty("selection").ValueKind == JsonValueKind.Null && finished.GetProperty("routeId").GetString() == routeId,
                "authoritative guide query returns null once all route targets are complete");
            await core.ExecuteRoutePlanningAsync("stop", new { profileId = profile, routeId });
            none = await Query();
            check(none.GetProperty("selection").ValueKind == JsonValueKind.Null && none.GetProperty("routeId").GetString() == "",
                "authoritative guide query returns null after stopping navigation despite the saved route remaining on disk");
            // This final marker response also drains earlier native marker events.
            await core.ExecuteMarkerAsync("markerGetSnapshot", new { profileId = profile });
            check(Volatile.Read(ref selectedEvents) == 0,
                "authoritative guide queries and completion updates never emit an uncorrelated markerSelected event");
            await core.ExecuteRoutePlanningAsync("delete", new { profileId = profile, routeId });
        }
        finally
        {
            core.MarkerEvent -= MarkerEvent;
            await core.ExecuteMarkerAsync("markerSelectProfile", new { profileId = "local" });
        }
    }

    private static async Task VerifyStopDeleteAndGuideAsync(CoreHostService core, string appData, string savedPoints,
        string activeId, string completedKey, string pendingKey, string otherProfile, Action<bool, string> check)
    {
        string routeFolder = Path.Combine(appData, "IMao-WinUI", "SavedRoutes", "Auto", "local");
        string routePath = Path.Combine(routeFolder, activeId + ".json");
        string pointerPath = Path.Combine(routeFolder, "active.json");
        string completionPath = Path.Combine(savedPoints, "profiles", "local.json");
        string completionBytes = await File.ReadAllTextAsync(completionPath);
        string routeBytes = await File.ReadAllTextAsync(routePath);

        // A guide request identifies the current point but never completes or skips it.
        var selected = new TaskCompletionSource<JsonElement>(TaskCreationOptions.RunContinuationsAsynchronously);
        void MarkerSelected(object? sender, JsonElement value)
        {
            if (value.TryGetProperty("type", out var type) && type.GetString() == "markerSelected") selected.TrySetResult(value.Clone());
        }
        core.MarkerEvent += MarkerSelected;
        try
        {
            var guided = await core.ExecuteRoutePlanningAsync("guide", new { routeId = activeId, key = pendingKey, profileId = "local" });
            var marker = await selected.Task.WaitAsync(TimeSpan.FromSeconds(10));
            check(marker.GetProperty("pointId").GetString() == pendingKey.Split(':')[1] &&
                marker.GetProperty("profileId").GetString() == "local" && guided.CurrentTarget?.Key == pendingKey &&
                await File.ReadAllTextAsync(completionPath) == completionBytes,
                "guide publishes the current target identity without changing navigation progress or marker completion");
        }
        finally { core.MarkerEvent -= MarkerSelected; }

        await core.ExecuteRoutePlanningAsync("new", new { sceneId = 1 });
        await core.ExecuteRoutePlanningAsync("setStart", new { sceneId = 1, x = 42, y = 24 });
        await core.ExecuteRoutePlanningAsync("add", new { keys = new[] { pendingKey } });
        var state = await GenerateAsync(core);
        string previewId = state.Preview!.Id;
        bool stopFailed;
        using (var held = new FileStream(pointerPath, FileMode.Open, FileAccess.Read, FileShare.Read))
            stopFailed = await RejectedAsync(() => core.ExecuteRoutePlanningAsync("stop", new { profileId = "local", routeId = activeId }));
        state = await core.ExecuteRoutePlanningAsync("state");
        check(stopFailed && state.Active?.Id == activeId && state.Preview?.Id == previewId &&
            state.CurrentTarget?.Key == pendingKey && await File.ReadAllTextAsync(routePath) == routeBytes,
            "failed durable stop preserves active navigation, draft preview and saved route bytes");

        state = await core.ExecuteRoutePlanningAsync("stop", new { profileId = "local", routeId = activeId });
        check(state.Active is null && state.Preview is null && !state.Enabled && !state.Computing && state.CurrentTarget is null &&
            state.SavedRoutes.Any(route => route.Id == activeId) && await File.ReadAllTextAsync(routePath) == routeBytes &&
            await File.ReadAllTextAsync(completionPath) == completionBytes,
            "stop removes active and preview navigation while retaining saved routes and authoritative completion");
        await core.ExecuteRoutePlanningAsync("stop", new { profileId = "local" });
        check(await RejectedAsync(() => core.ExecuteRoutePlanningAsync("guide")), "guide after stop does not reuse a stale target");
        await core.RestartAsync();
        state = await core.ExecuteRoutePlanningAsync("state");
        check(state.Active is null && state.Preview is null && !state.Computing && state.SavedRoutes.Any(route => route.Id == activeId) &&
            await File.ReadAllTextAsync(completionPath) == completionBytes,
            "stopped navigation stays stopped across host restart without erasing saved work or completion");

        state = await core.ExecuteRoutePlanningAsync("load", new { routeId = activeId });
        check(state.CurrentTarget?.Key == pendingKey && state.Active!.Stops.Single(stop => stop.Key == completedKey).Completed,
            "stopped route can be explicitly loaded later with its previous completion intact");
        state = await core.ExecuteRoutePlanningAsync("skip", new { routeId = activeId, key = pendingKey, profileId = "local" });
        check(state.NavigationStatus == "finished" && await RejectedAsync(() => core.ExecuteRoutePlanningAsync("guide")),
            "guide on an exhausted route is rejected instead of opening a completed or skipped point");
        await core.ExecuteRoutePlanningAsync("undoSkip");

        // Deletion of a separate saved preview and a corrupt file must preserve current navigation.
        await core.ExecuteRoutePlanningAsync("new", new { sceneId = 1 });
        await core.ExecuteRoutePlanningAsync("setStart", new { sceneId = 1, x = 10, y = 10 });
        await core.ExecuteRoutePlanningAsync("add", new { keys = new[] { pendingKey } });
        state = await GenerateAsync(core);
        string inactiveId = state.Preview!.Id;
        await core.ExecuteRoutePlanningAsync("save", new { target = "preview", name = "待删除的独立路线" });
        state = await core.ExecuteRoutePlanningAsync("delete", new { routeId = inactiveId, profileId = "local" });
        check(state.Active?.Id == activeId && state.CurrentTarget?.Key == pendingKey &&
            !state.SavedRoutes.Any(route => route.Id == inactiveId) && !File.Exists(Path.Combine(routeFolder, inactiveId + ".json")),
            "deleting an inactive saved route leaves the current route and target unchanged");
        const string corruptId = "corrupt-route-for-delete";
        string corruptPath = Path.Combine(routeFolder, corruptId + ".json");
        await File.WriteAllTextAsync(corruptPath, "{broken route");
        state = await core.ExecuteRoutePlanningAsync("list");
        check(state.SavedRoutes.Any(route => route.Id == corruptId), "corrupt saved route remains selectable for deletion");
        state = await core.ExecuteRoutePlanningAsync("delete", new { routeId = corruptId, profileId = "local" });
        check(!File.Exists(corruptPath) && state.Active?.Id == activeId && !state.SavedRoutes.Any(route => route.Id == corruptId),
            "corrupt saved route deletion succeeds without parsing its content or clearing the other active route");

        foreach (string invalidId in new[] { "missing-route", "../escape", "..\\escape", "active" })
        {
            bool invalidDelete = await RejectedAsync(() => core.ExecuteRoutePlanningAsync("delete", new { routeId = invalidId, profileId = "local" }));
            state = await core.ExecuteRoutePlanningAsync("state");
            check(invalidDelete && state.Active?.Id == activeId && state.CurrentTarget?.Key == pendingKey && File.Exists(routePath),
                "missing, reserved and traversing deletion requests never clear a different active route");
        }
        string beforeDelete = await File.ReadAllTextAsync(routePath);
        string beforePointer = await File.ReadAllTextAsync(pointerPath);
        bool deleteFailed;
        using (var held = new FileStream(routePath, FileMode.Open, FileAccess.Read, FileShare.Read))
            deleteFailed = await RejectedAsync(() => core.ExecuteRoutePlanningAsync("delete", new { routeId = activeId, profileId = "local" }));
        state = await core.ExecuteRoutePlanningAsync("state");
        check(deleteFailed && state.Active?.Id == activeId && state.CurrentTarget?.Key == pendingKey &&
            await File.ReadAllTextAsync(routePath) == beforeDelete && await File.ReadAllTextAsync(pointerPath) == beforePointer,
            "locked route deletion fails without clearing active memory or the durable pointer");
        using (var held = new FileStream(pointerPath, FileMode.Open, FileAccess.Read, FileShare.Read))
            deleteFailed = await RejectedAsync(() => core.ExecuteRoutePlanningAsync("delete", new { routeId = activeId, profileId = "local" }));
        state = await core.ExecuteRoutePlanningAsync("state");
        check(deleteFailed && state.Active?.Id == activeId && await File.ReadAllTextAsync(routePath) == beforeDelete &&
            await File.ReadAllTextAsync(pointerPath) == beforePointer && !File.Exists(routePath + ".deleting"),
            "failed pointer clear rolls active deletion back before reporting failure");

        // A second profile owns its own route; neither explicit stale context nor implicit paths can cross profiles.
        await core.ExecuteMarkerAsync("markerSelectProfile", new { profileId = otherProfile });
        await core.ExecuteRoutePlanningAsync("new", new { sceneId = 1, profileId = otherProfile });
        await core.ExecuteRoutePlanningAsync("setStart", new { sceneId = 1, x = 0, y = 0, profileId = otherProfile });
        await core.ExecuteRoutePlanningAsync("add", new { keys = new[] { completedKey }, profileId = otherProfile });
        state = await GenerateAsync(core);
        state = await core.ExecuteRoutePlanningAsync("activate", new { profileId = otherProfile });
        string otherId = state.Active!.Id;
        string otherPath = Path.Combine(appData, "IMao-WinUI", "SavedRoutes", "Auto", otherProfile, otherId + ".json");
        string otherBytes = await File.ReadAllTextAsync(otherPath);
        bool foreignDelete = await RejectedAsync(() => core.ExecuteRoutePlanningAsync("delete", new { routeId = activeId, profileId = "local" }), "档案");
        bool missingOtherRoute = await RejectedAsync(() => core.ExecuteRoutePlanningAsync("delete", new { routeId = activeId, profileId = otherProfile }));
        bool foreignStop = await RejectedAsync(() => core.ExecuteRoutePlanningAsync("stop", new { routeId = activeId, profileId = "local" }), "档案");
        state = await core.ExecuteRoutePlanningAsync("state");
        check(foreignDelete && missingOtherRoute && foreignStop && state.Active?.Id == otherId &&
            await File.ReadAllTextAsync(routePath) == beforeDelete && await File.ReadAllTextAsync(otherPath) == otherBytes,
            "stop and delete cannot cross the active profile or disturb either profile's saved route");
        await core.ExecuteMarkerAsync("markerSelectProfile", new { profileId = "local" });
        state = await core.ExecuteRoutePlanningAsync("delete", new { routeId = activeId.ToUpperInvariant(), profileId = "local" });
        check(state.Active is null && state.Preview is null && !state.Computing && !state.Enabled && !File.Exists(routePath) &&
            !state.SavedRoutes.Any(route => route.Id == activeId) && await File.ReadAllTextAsync(completionPath) == completionBytes &&
            await File.ReadAllTextAsync(otherPath) == otherBytes,
            "deleting the active route with a case-variant ID stops navigation and preserves completion and the other profile");
        await core.RestartAsync();
        state = await core.ExecuteRoutePlanningAsync("state");
        check(state.Active is null && !state.SavedRoutes.Any(route => route.Id == activeId) &&
            await File.ReadAllTextAsync(completionPath) == completionBytes,
            "deleted active route cannot return after host restart and does not undo marker completion");
        await core.ExecuteMarkerAsync("markerSelectProfile", new { profileId = otherProfile });
        state = await core.ExecuteRoutePlanningAsync("state");
        check(state.Active?.Id == otherId && state.NavigationStatus == "paused" && !state.CurrentTarget!.Completed,
            "other profile still restores its own route paused after deletion and restart of the original profile");
        await core.ExecuteRoutePlanningAsync("delete", new { routeId = otherId, profileId = otherProfile });
        await core.ExecuteMarkerAsync("markerSelectProfile", new { profileId = "local" });
    }

    private static async Task VerifyHotkeyConfigurationIpcAsync(CoreHostService core, string path, Action<bool, string> check)
    {
        check(core.Configuration is { NearestCompletionKey: 90, ManualRouteKey: 81, CurrentTargetGuideKey: 119,
            GuidePreviousImageKey: 33, GuideNextImageKey: 34 },
            "real host session begins with the compatible default hotkey bindings");
        bool accepted = await core.ConfigureAsync(nearestCompletionKey: 0, manualRouteKey: 65,
            currentTargetGuideKey: 118, guidePreviousImageKey: 48, guideNextImageKey: 49, mapEnabled: false, statusBarEnabled: false);
        var expected = core.Configuration;
        check(accepted && expected is { NearestCompletionKey: 0, ManualRouteKey: 65, CurrentTargetGuideKey: 118,
            GuidePreviousImageKey: 48, GuideNextImageKey: 49 } &&
            new RuntimeConfigurationStore(path).Read() == expected,
            "real host accepts and persists disabled completion and changed route, guide and paging hotkeys");
        await core.RestartAsync();
        check(core.IsConnected && core.Configuration == expected && new RuntimeConfigurationStore(path).Read() == expected,
            "host restart reapplies all five persisted hotkeys without resetting runtime preferences");
        string bytes = await File.ReadAllTextAsync(path);
        foreach (int key in new[] { 65, 48, 49, 27, 77, 121 })
        {
            bool rejected = !await core.ConfigureAsync(currentTargetGuideKey: key, mapEnabled: true, statusBarEnabled: true);
            check(rejected && core.IsConnected && core.Configuration == expected && await File.ReadAllTextAsync(path) == bytes,
                "service rejects duplicate or reserved hotkeys without partially persisting unrelated runtime settings");
        }
        foreach (int key in new[] { 65, 118, 49, 27, 77, 121 })
        {
            bool rejected = !await core.ConfigureAsync(guidePreviousImageKey: key, mapEnabled: true);
            check(rejected && core.IsConnected && core.Configuration == expected && await File.ReadAllTextAsync(path) == bytes,
                "service rejects conflicting or reserved image keys without publishing or persisting a partial update");
        }
        int changes = 0;
        void Changed(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
        { if (e.PropertyName == nameof(CoreHostService.Configuration)) changes++; }
        core.PropertyChanged += Changed;
        try
        {
            accepted = await core.ConfigureAsync(guidePreviousImageKey: 0, guideNextImageKey: 0);
            expected = expected with { GuidePreviousImageKey = 0, GuideNextImageKey = 0 };
            check(accepted && core.Configuration == expected && changes == 1 && new RuntimeConfigurationStore(path).Read() == expected,
                "disabling both paging shortcuts preserves other preferences and notifies the live guide exactly once");
            accepted = await core.ConfigureAsync(mapUpdateCycle: expected.MapUpdateCycle + 1);
            expected = expected with { MapUpdateCycle = expected.MapUpdateCycle + 1 };
            check(accepted && core.Configuration == expected,
                "unrelated partial runtime updates retain disabled paging shortcuts");
        }
        finally { core.PropertyChanged -= Changed; }
    }

    private static async Task VerifyNativeHotkeyConfigurationAsync(string hostDirectory, Action<bool, string> check)
    {
        string pipeName = "IMao.Test.Hotkeys." + Guid.NewGuid().ToString("N");
        using var process = Process.Start(new ProcessStartInfo
        {
            FileName = Path.Combine(hostDirectory, "IMao-CoreHost.exe"), Arguments = "--pipe " + pipeName,
            WorkingDirectory = hostDirectory, UseShellExecute = false, CreateNoWindow = true
        }) ?? throw new IOException("Could not start hotkey IPC test host");
        try
        {
            using var pipe = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
            await pipe.ConnectAsync(5000);
            using var reader = new StreamReader(pipe, new UTF8Encoding(false), leaveOpen: true);
            using var writer = new StreamWriter(pipe, new UTF8Encoding(false), leaveOpen: true) { AutoFlush = true };
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
            async Task<JsonElement> ReadAsync()
            {
                string line = await reader.ReadLineAsync(timeout.Token) ?? throw new IOException("Hotkey test host closed unexpectedly");
                using var document = JsonDocument.Parse(line);
                return document.RootElement.Clone();
            }
            async Task<bool> ConfigureAsync(Dictionary<string, object?> payload)
            {
                string id = Guid.NewGuid().ToString("N");
                payload["version"] = 1; payload["type"] = "configure"; payload["requestId"] = id;
                await writer.WriteLineAsync(JsonSerializer.Serialize(payload).AsMemory(), timeout.Token);
                while (true)
                {
                    var message = await ReadAsync();
                    if (message.TryGetProperty("requestId", out var request) && request.GetString() == id)
                        return message.GetProperty("accepted").GetBoolean();
                }
            }
            check(await ConfigureAsync(new() { ["nearestCompletionKey"] = 65, ["manualRouteKey"] = 66,
                ["currentTargetGuideKey"] = 117, ["statusBarEnabled"] = false, ["mapEnabled"] = true }),
                "native host accepts valid raw hotkey configuration independently of managed validation");
            check(!await ConfigureAsync(new() { ["guideNextImageKey"] = 33 }) &&
                !await ConfigureAsync(new() { ["currentTargetGuideKey"] = 34 }),
                "raw legacy configuration retains PageUp/PageDown defaults and checks their collisions with all actions");
            check(!await ConfigureAsync(new() { ["nearestCompletionKey"] = 67, ["manualRouteKey"] = 67,
                ["statusBarEnabled"] = true, ["mapEnabled"] = false }),
                "native host rejects duplicate hotkeys submitted over the raw protocol");
            for (int observed = 0; observed < 3;)
            {
                var message = await ReadAsync();
                if (message.GetProperty("type").GetString() != "status") continue;
                check(!message.GetProperty("statusBarEnabled").GetBoolean(),
                    "native hotkey rejection leaves the observable unrelated status preference unchanged");
                ++observed;
            }
            check(await ConfigureAsync(new() { ["manualRouteKey"] = 67 }),
                "valid partial update proves the rejected native command did not publish its nearest-key change");
            check(!await ConfigureAsync(new() { ["currentTargetGuideKey"] = 121, ["manualRouteKey"] = 68 }) &&
                !await ConfigureAsync(new() { ["nearestCompletionKey"] = 67 }),
                "reserved-key rejection leaves the previous native manual binding intact for later duplicate validation");
            check(await ConfigureAsync(new() { ["guidePreviousImageKey"] = 48, ["guideNextImageKey"] = 49 }),
                "raw protocol applies custom previous and next image keys together");
            check(!await ConfigureAsync(new() { ["guidePreviousImageKey"] = 49 }) &&
                !await ConfigureAsync(new() { ["currentTargetGuideKey"] = 48 }) &&
                !await ConfigureAsync(new() { ["guideNextImageKey"] = 67 }) &&
                !await ConfigureAsync(new() { ["guidePreviousImageKey"] = 121 }),
                "native paging configuration rejects duplicate previous/next keys, cross-action collisions and reserved keys");
            check(await ConfigureAsync(new() { ["guidePreviousImageKey"] = 0, ["guideNextImageKey"] = 0 }) &&
                await ConfigureAsync(new() { ["currentTargetGuideKey"] = 33, ["manualRouteKey"] = 34 }) &&
                !await ConfigureAsync(new() { ["guidePreviousImageKey"] = 33 }),
                "raw protocol permits both paging keys disabled and PageUp/PageDown assigned to other distinct actions");
        }
        finally
        {
            if (!process.HasExited)
            {
                using var shutdown = new CancellationTokenSource(TimeSpan.FromSeconds(5));
                try { await process.WaitForExitAsync(shutdown.Token); }
                catch (OperationCanceledException) { process.Kill(entireProcessTree: true); await process.WaitForExitAsync(); }
            }
        }
    }

    private static async Task<RoutePlanningState> GenerateAsync(CoreHostService core, string action = "generate")
    {
        var ready = new TaskCompletionSource<RoutePlanningState>(TaskCreationOptions.RunContinuationsAsynchronously);
        ulong generation = core.RoutePlanning.Generation;
        void Changed(object? sender, RoutePlanningState state)
        {
            if (!state.Computing && state.Preview is not null && state.Generation > generation) ready.TrySetResult(state);
        }
        core.RoutePlanningChanged += Changed;
        try
        {
            await core.ExecuteRoutePlanningAsync(action);
            Changed(null, core.RoutePlanning);
            return await ready.Task.WaitAsync(TimeSpan.FromSeconds(10));
        }
        finally { core.RoutePlanningChanged -= Changed; }
    }

    private static async Task<bool> RejectedAsync(Func<Task<RoutePlanningState>> action, string? expectedMessage = null)
    {
        try { await action(); return false; }
        catch (InvalidOperationException exception)
        { return expectedMessage is null || exception.Message.Contains(expectedMessage, StringComparison.Ordinal); }
    }
}
