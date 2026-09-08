using IMao_WinUI.Models;
using System.Text.Json;

namespace IMao_WinUI.Services;

public interface IMapToolsController
{
    bool IsOpen { get; }
    bool IsReturning { get; }
    bool ReturnFailed { get; }
    bool BlocksGuideInput { get; }
    event Action<string>? StatusChanged;
    event Action? Ended;
    Task OpenAsync(JsonElement context);
    Task CloseAsync(string reason, bool restoreGame);
    void Feed(GamepadSample sample, long now);
    GamepadHandoffLease? AcquireHandoff(nint source);
}
