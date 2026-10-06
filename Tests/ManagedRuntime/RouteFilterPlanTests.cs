using IMao_WinUI.Services;

internal static class RouteFilterPlanTests
{
    public static void Run(Action<bool, string> check)
    {
        var current = new Dictionary<string, bool>(StringComparer.Ordinal)
        {
            ["dingdingdong"] = true, ["chest"] = true, ["boss"] = false, ["herb"] = true
        };

        // The route visits 叮叮咚 and BOSS. 叮叮咚 stays, BOSS has to come back, and the two types
        // the player was looking at but the route never touches have to go.
        var plan = RouteFilterPlan.Narrow(current, ["dingdingdong", "boss"]);
        check(plan.Disable.OrderBy(id => id, StringComparer.Ordinal).SequenceEqual(["chest", "herb"]),
            "applying a route hides exactly the point types the route does not visit");
        check(plan.Enable.SequenceEqual(["boss"]),
            "a point type the route visits is shown again if the player had hidden it");
        check(!plan.Disable.Contains("boss") && !plan.Enable.Contains("dingdingdong"),
            "a type that is already in the wanted state is left out of the request entirely");
        check(plan.Kept == 2, "the report counts the types the route actually uses");

        // Free points carry no type, so a hand-drawn route made only of them would ask for the
        // whole map to be hidden. The plan says so honestly — the caller is what refuses to act on
        // it, so that "a route with no types" can never switch every layer off.
        var freeOnly = RouteFilterPlan.Narrow(current, []);
        check(freeOnly.Kept == 0 && freeOnly.Enable.Count == 0 && freeOnly.Disable.Count == 3,
            "a route with no point types would hide everything, which is why the caller must not apply it");
        check(current["dingdingdong"] && current["herb"],
            "deciding what a route needs does not modify the filter state it was given");

        // Restoring must send only what moved, because the native filter registry appends whatever
        // it is given. 叮叮咚 was already shown and stays shown, so it must not be in the request.
        var afterNarrow = new Dictionary<string, bool>(StringComparer.Ordinal)
        {
            ["dingdingdong"] = true, ["chest"] = false, ["boss"] = true, ["herb"] = false
        };
        var restore = RouteFilterPlan.Restore(current, afterNarrow);
        check(restore.OrderBy(entry => entry.Id, StringComparer.Ordinal).Select(entry => entry.Id)
                .SequenceEqual(["boss", "chest", "herb"]),
            "restoring names every type that moved and leaves the untouched ones out: got [" +
            string.Join(",", restore.Select(entry => entry.Id)) + "]");
        check(restore.All(entry => entry.Enabled == current[entry.Id]),
            "restoring puts each type back to the state the player had before the route was applied");

        var unchanged = RouteFilterPlan.Restore(current, new Dictionary<string, bool>(current, StringComparer.Ordinal));
        check(unchanged.Count == 0, "a filter that never moved is not re-sent when the route filter is switched off");

        // A type the catalogue knows but the rows no longer carry still has to be restorable.
        var shrunk = new Dictionary<string, bool>(StringComparer.Ordinal) { ["dingdingdong"] = true };
        check(RouteFilterPlan.Restore(current, shrunk).Count == 3,
            "a type missing from the current rows is still restored from the remembered set");

        // Switching from one route to another narrows what is already narrowed. The memory handed to
        // Restore has to be the state from *before the first route*, or the second route's filter
        // would be remembered as "the original" and the player could never get their own map back.
        var beforeAnyRoute = new Dictionary<string, bool>(StringComparer.Ordinal)
        {
            ["dingdingdong"] = true, ["chest"] = true, ["boss"] = true, ["herb"] = true
        };
        var afterFirstRoute = new Dictionary<string, bool>(StringComparer.Ordinal)
        {
            ["dingdingdong"] = true, ["chest"] = false, ["boss"] = false, ["herb"] = false
        };
        var secondRoute = RouteFilterPlan.Narrow(afterFirstRoute, ["boss"]);
        check(secondRoute.Enable.SequenceEqual(["boss"]) && secondRoute.Disable.SequenceEqual(["dingdingdong"]),
            "narrowing to a second route hides what the first route showed and shows what this one needs: enable=[" +
            string.Join(",", secondRoute.Enable) + "] disable=[" + string.Join(",", secondRoute.Disable) + "]");
        var restoreAfterTwo = RouteFilterPlan.Restore(beforeAnyRoute,
            new Dictionary<string, bool>(StringComparer.Ordinal)
            {
                ["dingdingdong"] = false, ["chest"] = false, ["boss"] = true, ["herb"] = false
            });
        // 叮叮咚 ends up enabled again by the second route, so it never moved away from the state the
        // player had and is not re-sent; the other three are.
        check(restoreAfterTwo.Count == 3 && restoreAfterTwo.All(entry => entry.Enabled),
            "restoring after two routes puts back the map the player had before any route: [" +
            string.Join(",", restoreAfterTwo.Select(entry => entry.Id + "=" + entry.Enabled)) + "]");

        VerifySnapshot(check);
    }

    /// <summary>
    /// The borrowed filter has to survive the process, because the filter it replaced was saved: an
    /// app closed mid-route must not wake up believing the narrowed map is the player's own choice.
    /// </summary>
    private static void VerifySnapshot(Action<bool, string> check)
    {
        string folder = Path.Combine(Path.GetTempPath(), "imao-route-filter-loan-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(folder);
        string original = RouteFilterSnapshot.Path;
        try
        {
            RouteFilterSnapshot.Path = Path.Combine(folder, "RouteFilterLoan.json");
            check(RouteFilterSnapshot.Load() is null, "no loan is in effect before any route is applied");

            var mine = new Dictionary<string, bool>(StringComparer.Ordinal)
            {
                ["dingdingdong"] = true, ["chest"] = false, ["boss"] = true
            };
            RouteFilterSnapshot.Begin("route-a", mine);
            var stored = RouteFilterSnapshot.Load();
            check(stored is { } first && first.RouteId == "route-a" && first.Enabled.Count == 3 &&
                first.Enabled["chest"] == false && first.Enabled["boss"],
                "applying a route remembers the exact map the player had");

            // A second route must not overwrite the memory: the loan still belongs to the player's
            // original map, not to the first route's.
            RouteFilterSnapshot.Begin("route-b", new Dictionary<string, bool>(StringComparer.Ordinal) { ["boss"] = false });
            check(RouteFilterSnapshot.Load() is { } second && second.RouteId == "route-b" && second.Enabled.Count == 3,
                "a second route updates loan ownership while preserving the original map");

            RouteFilterSnapshot.End();
            check(RouteFilterSnapshot.Load() is null, "ending the loan clears it, so nothing is restored twice");

            // A damaged document must not be mistaken for a loan, or the player would be handed a
            // filter they never had.
            File.WriteAllText(RouteFilterSnapshot.Path, "{ not json");
            check(RouteFilterSnapshot.Load() is null, "a damaged loan document is ignored rather than applied");
            RouteFilterSnapshot.End();
        }
        finally
        {
            RouteFilterSnapshot.Path = original;
            try { Directory.Delete(folder, recursive: true); } catch (IOException) { }
        }
    }
}
