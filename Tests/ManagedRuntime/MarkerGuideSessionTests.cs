using IMao_WinUI.Models;

internal static class MarkerGuideSessionTests
{
    public static async Task RunAsync(Action<bool, string> check)
    {
        var first = new MarkerSelection { ProfileId = "local", StateId = 8, PointId = "1409977912641277952", NameId = "sx_qq" };
        var second = first with { PointId = "1409980210964680704" };
        var session = new MarkerGuideSession();
        check(!session.IsOpen && session.Selection is null && !session.Close(),
            "guide session starts closed and repeated close has no side effects");
        for (int cycle = 0; cycle < 20; ++cycle)
        {
            long opened = session.Open(first);
            check(session.IsOpen && session.IsCurrent(opened) && session.Selection == first && session.Close(opened) &&
                !session.IsOpen && session.Selection is null && !session.IsCurrent(opened),
                "repeated guide shortcut open and close invalidates each previous presentation");
        }

        foreach (string phase in new[] { "nearby candidate lookup", "window registration", "local details", "online details" })
        {
            long pendingGeneration = session.Open(first);
            var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            async Task<bool> CanResumeAsync()
            {
                await release.Task;
                return session.IsCurrent(pendingGeneration);
            }
            Task<bool> pending = CanResumeAsync();
            check(session.Close(), "guide can close while awaiting " + phase);
            long replacement = session.Open(second);
            release.SetResult();
            check(!await pending && session.IsCurrent(replacement) && session.Selection == second && !session.Close(pendingGeneration),
                "late " + phase + " continuation and old window close cannot revive or close the replacement guide");
        }

        long generation = session.Open(first);
        check(session.ApplyCompletion(generation, first.ProfileId, first.StateId, first.PointId, true, accepted: false) ==
                MarkerGuideCompletionResult.Ignored && session.IsCurrent(generation) && session.Selection == first,
            "failed completion keeps the guide open with the previous completion state");
        foreach (var mismatch in new[] { second, first with { ProfileId = "other" }, first with { StateId = 9 } })
            check(session.ApplyCompletion(generation, mismatch.ProfileId, mismatch.StateId, mismatch.PointId, true) ==
                    MarkerGuideCompletionResult.Ignored && session.IsCurrent(generation) && session.Selection == first,
                "completion of another point, profile or map does not close or change the current guide");

        check(session.ApplyCompletion(generation, first.ProfileId, first.StateId, first.PointId, true) ==
                MarkerGuideCompletionResult.Closed && !session.IsOpen && session.Selection is null && !session.IsCurrent(generation),
            "successful completion of the current point closes the guide and invalidates its pending work");
        long reopened = session.Open(first);
        check(session.ApplyCompletion(generation, first.ProfileId, first.StateId, first.PointId, true) ==
                MarkerGuideCompletionResult.Ignored && session.IsCurrent(reopened) && !session.Close(generation),
            "a repeated old completion and old close cannot dismiss a newly reopened guide for the same point");

        // Requests may finish after switching away and back to the exact same point/profile.
        long beforeSwitch = session.Open(first);
        session.Open(second);
        session.Open(first with { ProfileId = "other" });
        long afterSwitch = session.Open(first);
        check(session.ApplyCompletion(beforeSwitch, first.ProfileId, first.StateId, first.PointId, true) ==
                MarkerGuideCompletionResult.Ignored && session.IsCurrent(afterSwitch),
            "selection round trips do not let an old completion acknowledgement close the new same-point guide");
        long undoGeneration = session.Open(first with { Completed = true });
        check(session.ApplyCompletion(undoGeneration, first.ProfileId, first.StateId, first.PointId, false) ==
                MarkerGuideCompletionResult.Updated && session.IsCurrent(undoGeneration) && session.Selection is { Completed: false },
            "successful undo updates the current marker and keeps its guide open");
        check(session.ApplyCompletion(undoGeneration, first.ProfileId, first.StateId, first.PointId, true, accepted: false) ==
                MarkerGuideCompletionResult.Ignored && session.IsCurrent(undoGeneration) && session.Selection is { Completed: false },
            "a later failed completion does not undo the displayed successful undo or close its guide");
        check(session.ApplyCompletion(undoGeneration, first.ProfileId, first.StateId, first.PointId, true, closeOnComplete: false) ==
                MarkerGuideCompletionResult.Updated && session.IsCurrent(undoGeneration) && session.Selection is { Completed: true },
            "completion synchronization can update the current guide without treating it as a new close action");
        long synchronized = session.Open(first);
        check(session.ApplyCompletion(undoGeneration, first.ProfileId, first.StateId, first.PointId, true, closeOnComplete: false) ==
                MarkerGuideCompletionResult.Ignored && session.IsCurrent(synchronized) && session.Selection is { Completed: false },
            "nonclosing completion synchronization still rejects obsolete presentation generations");
        session.Close();
        check(session.ApplyCompletion(session.Generation, first.ProfileId, first.StateId, first.PointId, true) ==
                MarkerGuideCompletionResult.Ignored && !session.IsOpen,
            "a completion event received with no open guide cannot create a presentation");
    }
}
