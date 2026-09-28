namespace IMao_WinUI.Services;

/// <summary>
/// What applying a route does to the map filter: keep the point types the route visits, hide the
/// rest, and be able to put everything back.
///
/// The decision is separated from the service that carries it out because the interesting part is
/// what is *left out* of the request. The native filter registry only ever appends the ids it is
/// given, so a filter must be expressed as a difference — sending the full set every time would
/// grow that registry without bound.
/// </summary>
public static class RouteFilterPlan
{
    public sealed record Narrowed(IReadOnlyList<string> Enable, IReadOnlyList<string> Disable, int Kept);

    /// <summary>
    /// The difference between what is currently shown and what the route needs.
    /// <paramref name="current"/> maps every known point type to whether it is shown.
    /// </summary>
    public static Narrowed Narrow(IReadOnlyDictionary<string, bool> current, IEnumerable<string> kinds)
    {
        ArgumentNullException.ThrowIfNull(current);
        ArgumentNullException.ThrowIfNull(kinds);
        var wanted = new HashSet<string>(kinds.Where(kind => !string.IsNullOrWhiteSpace(kind)), StringComparer.Ordinal);
        var enable = new List<string>();
        var disable = new List<string>();
        foreach (var (id, shown) in current)
        {
            if (wanted.Contains(id))
            {
                // A type the route visits that is currently hidden has to come back, or the route
                // would run through points the player cannot see.
                if (!shown) enable.Add(id);
            }
            else if (shown) disable.Add(id);
        }
        return new Narrowed(enable, disable, wanted.Count);
    }

    /// <summary>
    /// The requests that put the filter back as it was. Only the types that actually moved are
    /// sent, for the same reason as above — and because the service already skips ids whose state
    /// matches, so a redundant request here would be a no-op that still had to cross the pipe.
    /// </summary>
    public static IReadOnlyList<(string Id, bool Enabled)> Restore(
        IReadOnlyDictionary<string, bool> previous, IReadOnlyDictionary<string, bool> current)
    {
        ArgumentNullException.ThrowIfNull(previous);
        ArgumentNullException.ThrowIfNull(current);
        var restore = new List<(string, bool)>();
        foreach (var (id, was) in previous)
        {
            if (current.TryGetValue(id, out var now) && now != was) restore.Add((id, was));
            else if (!current.ContainsKey(id)) restore.Add((id, was));
        }
        return restore;
    }
}
