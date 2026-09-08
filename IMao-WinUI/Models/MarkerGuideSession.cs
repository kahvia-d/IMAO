namespace IMao_WinUI.Models;

public enum MarkerGuideCompletionResult { Ignored, Updated, Closed }

// Owned by the UI thread. A generation spans registration, loading and completion requests.
public sealed class MarkerGuideSession
{
    public long Generation { get; private set; }
    public bool IsOpen { get; private set; }
    public MarkerSelection? Selection { get; private set; }
    public string? ProfileId { get; private set; }

    public long OpenPending(string profileId)
    {
        Generation++;
        ProfileId = profileId;
        Selection = null;
        IsOpen = true;
        return Generation;
    }

    public bool SetSelection(long generation, MarkerSelection selection)
    {
        if (!IsCurrent(generation) || selection.ProfileId != ProfileId) return false;
        Selection = selection;
        return true;
    }

    // Call before awaiting registration so a second shortcut can close an opening guide.
    public long Open(MarkerSelection selection)
    {
        ArgumentNullException.ThrowIfNull(selection);
        long generation = OpenPending(selection.ProfileId);
        SetSelection(generation, selection);
        return generation;
    }

    public bool IsCurrent(long generation) => IsOpen && generation == Generation;

    public bool Close(long? expectedGeneration = null)
    {
        if (!IsOpen || expectedGeneration is { } expected && expected != Generation) return false;
        Generation++;
        IsOpen = false;
        Selection = null;
        ProfileId = null;
        return true;
    }

    public MarkerGuideCompletionResult ApplyCompletion(long generation, string profileId, int stateId,
        string pointId, bool completed, bool accepted = true, bool closeOnComplete = true)
    {
        if (!accepted || !IsCurrent(generation) || Selection is not { } selection ||
            selection.ProfileId != profileId || selection.StateId != stateId || selection.PointId != pointId)
            return MarkerGuideCompletionResult.Ignored;
        Selection = selection with { Completed = completed };
        if (completed && closeOnComplete)
        {
            Close(generation);
            return MarkerGuideCompletionResult.Closed;
        }
        return MarkerGuideCompletionResult.Updated;
    }
}
