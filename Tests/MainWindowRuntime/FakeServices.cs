using System.Collections.ObjectModel;
using System.ComponentModel;
using CommunityToolkit.Mvvm.ComponentModel;
using IMao_WinUI.Contracts.Services;
using IMao_WinUI.Models;
using Microsoft.UI.Xaml;
namespace IMao_WinUI
{
    public static class App
    {
        public static Window MainWindow { get; set; } = null!;
        public static UIElement? AppTitlebar { get; set; }
        public static Dictionary<Type,object> Services { get; } = new();
        public static T GetService<T>() where T:class => (T)Services[typeof(T)];
    }
}
namespace IMao_WinUI.ViewModels
{
    public class SettingsViewModel: ObservableObject { public string VersionDescription => "IMao · 独立界面测试"; }
}
namespace IMao_WinUI.Helpers
{
    public static class GameWindow { public static bool CheckGameWindowSize() => true; }
}
namespace IMao_WinUI.StringItems
{
    public sealed record FixtureItem(string Id,string Name_SpecifiedLanguage);
    public sealed record FixtureGroup(string Category,List<FixtureItem> ItemDatas);
    public sealed class StringItem
    {
        public List<FixtureGroup> itemsDatas { get; } = new();
        public void LoadString(string language) => itemsDatas.Add(new("收集物",Enumerable.Range(1,600).Select(i=>new FixtureItem("point"+i, i % 4 == 0 ? "非常长的测试探索点位名称"+i : "探索收集物 "+i)).ToList()));
    }
}
namespace IMao_WinUI.Services
{
    /// <summary>
    /// In-memory stand-in for the real local settings store. The pages only need the setting to come back
    /// in the same session, and the fixture must not write to the user's own settings file.
    /// </summary>
    public sealed class FixtureLocalSettingsService : ILocalSettingsService
    {
        private readonly Dictionary<string, object?> values = new();
        public Task<T?> ReadSettingAsync<T>(string key) =>
            Task.FromResult(values.TryGetValue(key, out var found) && found is T typed ? typed : default);
        public Task SaveSettingAsync<T>(string key, T value) { values[key] = value; return Task.CompletedTask; }
    }
    public sealed record FixtureLog(string DisplayText);
    public enum ExplorationToggleResult { Started, Stopped, WindowSizeRejected }
    public sealed class GamepadInputService : INotifyPropertyChanged
    {
        public event PropertyChangedEventHandler? PropertyChanged;
        public string StatusMessage => "测试手柄：已连接";
        public void SetConfigurationPending(bool pending) => PropertyChanged?.Invoke(this,new(nameof(StatusMessage)));
    }
    /// <summary>
    /// The pages talk to the shell through this one type, so the fixture has to answer with the same surface
    /// the real host does - but with every answer coming from memory. Nothing here starts the native core,
    /// opens the pipe, polls a controller or reads the user's own settings. The record books are a real
    /// <see cref="LocalAccountCatalog"/> over a temporary directory instead of the user's, because the pages
    /// render its rows and the fixture should exercise that path rather than a stub of it.
    /// </summary>
    public sealed class CoreHostService : INotifyPropertyChanged
    {
        public event PropertyChangedEventHandler? PropertyChanged;
        public event EventHandler<CoreRuntimeStatus>? StatusChanged;
        public event EventHandler<RoutePlanningState>? RoutePlanningChanged;
        public event EventHandler<System.Text.Json.JsonElement>? MarkerEvent;
        public RuntimeConfiguration Configuration { get; private set; } = new();
        public bool IsConnected { get; private set; } = true;
        public Task<bool> SynchronizeFilterAsync(IReadOnlyDictionary<string,bool> values,CancellationToken cancellationToken=default) => Task.FromResult(IsConnected);
        public void PublishConnection(bool connected) { IsConnected=connected; PropertyChanged?.Invoke(this,new(nameof(IsConnected))); }
        public CoreRuntimeStatus Status { get; private set; } = new() { CoreState="ready",Message="游戏与地图服务已就绪，点击开始探索。",CoreVersion="fixture" };
        public RoutePlanningState RoutePlanning { get; private set; } = new();
        public ObservableCollection<FixtureLog> RecentLogs { get; } = new(Enumerable.Range(1,400).Select(i=>new FixtureLog($"20:30:{i%60:D2}  [状态]  测试事件 {i} · 地图定位和显示工作正常")));
        public string LastFault { get; private set; } = "";
        public string LogDirectory => Path.Combine(AppContext.BaseDirectory,"fixture","logs");
        public string CrashDirectory => Path.Combine(AppContext.BaseDirectory,"fixture","crashes");
        public bool RejectConfigure { get; set; }
        public List<(string Action,object? Arguments)> RouteCommands { get; } = new();
        public List<(string Operation,object? Arguments)> MarkerCommands { get; } = new();
        /// <summary>Record books over a throwaway directory: the fixture must never touch the user's own.</summary>
        public LocalAccountCatalog? LedgerCatalog { get; } = new(
            Path.Combine(AppContext.BaseDirectory,"fixture","accounts.json"),
            Path.Combine(AppContext.BaseDirectory,"fixture","points"),
            routesDirectory: Path.Combine(AppContext.BaseDirectory,"fixture","routes"));
        public LocalAccount? ActiveLocalAccount => LedgerCatalog?.Active;
        public void ReportGamepadDiagnostic(string message, string details = "") { }
        public Task EnsureStartedAsync() => Task.CompletedTask;
        public Task StartRuntimeAsync() { PublishStatus(new() { CoreState="running",Message="地图叠加正在运行",MapMarkers=178,MinimapMarkers=12 }); return Task.CompletedTask; }
        public Task StopRuntimeAsync() { PublishStatus(new() { CoreState="ready",Message="探索已暂停。" });return Task.CompletedTask; }
        public Task RestartAsync() => StopRuntimeAsync();
        public Task<ExplorationToggleResult> ToggleExplorationAsync(bool canStart = true, CancellationToken cancellationToken = default) =>
            Task.FromResult(IsConnected && canStart ? ExplorationToggleResult.Started : ExplorationToggleResult.WindowSizeRejected);
        public void PublishStatus(CoreRuntimeStatus value) { Status=value; StatusChanged?.Invoke(this,value); PropertyChanged?.Invoke(this,new(nameof(Status))); }
        public void PublishRoute(RoutePlanningState value) { RoutePlanning=value; RoutePlanningChanged?.Invoke(this,value); }
        public void ReportUserError(string message) => LastFault=message;
        public Task SetItemsEnabledAsync(IEnumerable<string> ids,bool enabled) => Task.CompletedTask;
        public Task SetDiagnosticsCaptureAsync(bool enabled) => Task.CompletedTask;
        public Task SetOverlayHiddenAsync(bool enabled) => Task.CompletedTask;
        public Task SetHoldOverlayPresentAsync(bool enabled) => Task.CompletedTask;
        public Task SetIsolationSwitchesAsync(int mask) => Task.CompletedTask;
        public Task<RoutePlanningState> ExecuteRoutePlanningAsync(string action,object? arguments=null,CancellationToken cancellationToken=default)
        { RouteCommands.Add((action,arguments)); return Task.FromResult(RoutePlanning); }
        /// <summary>Marker commands are only logged: a page rendering its lists must not mutate any progress.</summary>
        public Task<System.Text.Json.JsonElement> ExecuteMarkerAsync(string operation, object? arguments = null, CancellationToken cancellationToken = default)
        { MarkerCommands.Add((operation,arguments)); return Task.FromResult(EmptyJson()); }
        public Task<System.Text.Json.JsonElement> ExecuteConnectedMarkerAsync(string operation, object? arguments = null, CancellationToken cancellationToken = default) =>
            ExecuteMarkerAsync(operation, arguments, cancellationToken);
        public Task<System.Text.Json.JsonElement> ImportLegacyPointsAsync(CancellationToken cancellationToken = default) =>
            Task.FromResult(System.Text.Json.JsonSerializer.SerializeToElement(new { imported = 0, alreadyCompleted = 0, skipped = 0 }));
        private static System.Text.Json.JsonElement EmptyJson() => System.Text.Json.JsonSerializer.SerializeToElement(new { });
        public Task<bool> ConfigureAsync(int? captureWay=null,int? overlayPresentMode=null,int? mapUpdateCycle=null,int? minMapUpdateCycle=null,bool? mapEnabled=null,bool? minMapEnabled=null,bool? savedPointsEnabled=null,bool? statusBarEnabled=null,bool? mapStatusBarEnabled=null,bool? statusBallEnabled=null,CancellationToken cancellationToken=default,int? nearestCompletionKey=null,int? manualRouteKey=null,int? currentTargetGuideKey=null,int? guideSkipKey=null,int? guidePreviousImageKey=null,int? guideNextImageKey=null,int? toggleEnabledKey=null,bool? gamepadEnabled=null,int? gamepadControllerIndex=null,GamepadButtons? gamepadEntryButton=null,bool? autoReplanEnabled=null,bool? expectedAutoReplanEnabled=null,string? expectedAutoReplanProfile=null,int? completionRangePixels=null,int? guideRangePixels=null,int? farmRangePixels=null)
        {
            if(RejectConfigure) { LastFault="测试保存拒绝"; return Task.FromResult(false); }
            var next=Configuration with {
                CaptureWay=captureWay??Configuration.CaptureWay,OverlayPresentMode=overlayPresentMode??Configuration.OverlayPresentMode,MapUpdateCycle=mapUpdateCycle??Configuration.MapUpdateCycle,MinMapUpdateCycle=minMapUpdateCycle??Configuration.MinMapUpdateCycle,
                MapEnabled=mapEnabled??Configuration.MapEnabled,MinMapEnabled=minMapEnabled??Configuration.MinMapEnabled,SavedPointsEnabled=savedPointsEnabled??Configuration.SavedPointsEnabled,StatusBarEnabled=statusBarEnabled??Configuration.StatusBarEnabled,
                MapStatusBarEnabled=mapStatusBarEnabled??Configuration.MapStatusBarEnabled,StatusBallEnabled=statusBallEnabled??Configuration.StatusBallEnabled,
                NearestCompletionKey=nearestCompletionKey??Configuration.NearestCompletionKey,ManualRouteKey=manualRouteKey??Configuration.ManualRouteKey,CurrentTargetGuideKey=currentTargetGuideKey??Configuration.CurrentTargetGuideKey,GuideSkipKey=guideSkipKey??Configuration.GuideSkipKey,GuidePreviousImageKey=guidePreviousImageKey??Configuration.GuidePreviousImageKey,GuideNextImageKey=guideNextImageKey??Configuration.GuideNextImageKey,
                ToggleEnabledKey=toggleEnabledKey??Configuration.ToggleEnabledKey,
                CompletionRangePixels=completionRangePixels??Configuration.CompletionRangePixels,GuideRangePixels=guideRangePixels??Configuration.GuideRangePixels,FarmRangePixels=farmRangePixels??Configuration.FarmRangePixels,
                GamepadEnabled=gamepadEnabled??Configuration.GamepadEnabled,GamepadControllerIndex=gamepadControllerIndex??Configuration.GamepadControllerIndex,AutoReplanEnabled=autoReplanEnabled??Configuration.AutoReplanEnabled };
            next.Validate();Configuration=next;PropertyChanged?.Invoke(this,new(nameof(Configuration)));
            PublishRoute(RoutePlanning with { AutoReplanEnabled=next.AutoReplanEnabled });
            return Task.FromResult(true);
        }
    }
}
