using System.ComponentModel;
using System.Text.Json;
using IMao_WinUI.Models;

// Only external services are faked. The production page, progress reader and XAML run verbatim.
namespace IMao_WinUI
{
    public static class App
    {
        public static readonly Dictionary<Type, object> Services = new();
        public static T GetService<T>() => (T)Services[typeof(T)];
    }
}
namespace IMao_WinUI.ViewModels { public sealed class StartViewModel { } }
namespace IMao_WinUI.Helpers
{
    public static class ResourceSessionPaths
    {
        public static string MapDataRoot { get; set; } = "";
        public static string MapIconRoot => MapDataRoot;
    }
}
namespace IMao_WinUI.Services
{
    public enum ExplorationToggleResult { Success, WindowSizeRejected }
    public sealed record LocalAccount(string Id, string Name, bool IsBound = false, string KuroAccountId = "");
    public sealed class LocalAccountCatalog(string directory)
    {
        public string ActiveId { get; set; } = "a";
        public string ProgressPath(string id) => Path.Combine(directory, id + ".json");
    }
    public sealed class CoreHostService
    {
        public event EventHandler<CoreRuntimeStatus>? StatusChanged;
        public event EventHandler<RoutePlanningState>? RoutePlanningChanged;
        public event PropertyChangedEventHandler? PropertyChanged;
        public event EventHandler<JsonElement>? MarkerEvent;
        public CoreRuntimeStatus Status { get; } = new() { CoreState = "ready" };
        public RoutePlanningState RoutePlanning { get; } = new();
        public LocalAccountCatalog? LedgerCatalog { get; set; }
        public LocalAccount? ActiveLocalAccount => new(LedgerCatalog!.ActiveId, LedgerCatalog.ActiveId);
        public object? SelectedCaptureTarget => null;
        public Task EnsureStartedAsync() => Task.CompletedTask;
        public Task RestartAsync() => Task.CompletedTask;
        public Task<ExplorationToggleResult> ToggleExplorationAsync(bool canStart) => Task.FromResult(ExplorationToggleResult.Success);
        public void ReportUserError(string message) => throw new Exception(message);
        public void Heartbeat() => StatusChanged?.Invoke(this, Status);
        public void SelectLedger(string id)
        {
            LedgerCatalog!.ActiveId = id;
            PropertyChanged?.Invoke(this, new(nameof(LedgerCatalog)));
        }
        public void CompletionChanged()
        {
            using var json = JsonDocument.Parse("{\"type\":\"markerCompletionChanged\"}");
            MarkerEvent?.Invoke(this, json.RootElement);
        }
    }
}
namespace IMao_WinUI.Views
{
    public static class CaptureWindowDialog
    {
        public static Task ShowAsync(Microsoft.UI.Xaml.XamlRoot root, Services.CoreHostService service) => Task.CompletedTask;
    }
}
public enum GameWindowCheckResult { Ready, WindowTooSmall }
public static class GameWindow
{
    public static GameWindowCheckResult InspectGameWindow(out nint handle) { handle = 0; return GameWindowCheckResult.Ready; }
    public static (string, string) DescribeFailure(GameWindowCheckResult result) => ("fixture", "fixture");
}
