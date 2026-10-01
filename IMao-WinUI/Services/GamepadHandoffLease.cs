namespace IMao_WinUI.Services;

/// <summary>
/// One attempt at handing the foreground from an input-owning window to a guide window. Completing it
/// reports the window that took over; disposing it reports that nothing did, which is what lets the caller
/// put the player back in the game instead of leaving a transparent window owning the foreground.
/// </summary>
/// <remarks>
/// It lives in its own file because <see cref="IMapToolsController.AcquireHandoff"/> names it in its
/// signature: while it was declared inside RouteGamepadController.cs, anything that compiled the interface
/// without that controller - the desktop harness, for one - could not compile the interface either.
/// </remarks>
public sealed class GamepadHandoffLease(Action<nint, bool> finished) : IDisposable
{
    private bool done;
    public void Complete(nint target) { if (!done) { done = true; finished(target, true); } }
    public void Dispose() { if (!done) { done = true; finished(0, false); } }
}
