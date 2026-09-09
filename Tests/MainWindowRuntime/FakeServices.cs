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
    public static class StringExtensions { public static string GetLocalized(this string value) => value == "AppDisplayName" ? "IMao" : value; }
    public static class TitleBarHelper { public static void UpdateTitleBar(ElementTheme theme) {} }
    public static class GameWindow { public static bool CheckGameWindowSize() => true; }
    public static class BitBltRegistryHelper { public static bool TryDisableSwapEffectUpgrade(out string error) { error = ""; return true; } }
    public static class UserDataPaths
    {
        public static string Root => Path.Combine(AppContext.BaseDirectory,"fixture");
        public static string SavedRoutes => Path.Combine(AppContext.BaseDirectory,"fixture","routes");
        public static string SavedPoints => Path.Combine(AppContext.BaseDirectory,"fixture","points");
    }
    public sealed record FilterItemDatas(string Name,int Status);
    public sealed class LocalItemFilter
    {
        public static Dictionary<string,int> Saved { get; } = new();
        public string LastError => "";
        public List<FilterItemDatas> GetFilteredItemsDatas() => Saved.Select(p=>new FilterItemDatas(p.Key,p.Value)).ToList();
        public bool SetItemsStatus(IEnumerable<string> ids,int status) { foreach(var id in ids) Saved[id]=status;return true; }
    }
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
    public sealed class GamepadInputService : INotifyPropertyChanged
    {
        public event PropertyChangedEventHandler? PropertyChanged;
        public string StatusMessage => "测试手柄：已连接";
        public void SetConfigurationPending(bool pending) => PropertyChanged?.Invoke(this,new(nameof(StatusMessage)));
    }
    public sealed record FixtureLog(string DisplayText);
    public sealed class CoreHostService : INotifyPropertyChanged
    {
        public event PropertyChangedEventHandler? PropertyChanged;
        public event EventHandler<CoreRuntimeStatus>? StatusChanged;
        public event EventHandler<RoutePlanningState>? RoutePlanningChanged;
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
        public Task EnsureStartedAsync() => Task.CompletedTask;
        public Task StartRuntimeAsync() { PublishStatus(new() { CoreState="running",Message="地图叠加正在运行",MapMarkers=178,MinimapMarkers=12 }); return Task.CompletedTask; }
        public Task StopRuntimeAsync() { PublishStatus(new() { CoreState="ready",Message="探索已暂停。" });return Task.CompletedTask; }
        public Task RestartAsync() => StopRuntimeAsync();
        public void PublishStatus(CoreRuntimeStatus value) { Status=value; StatusChanged?.Invoke(this,value); PropertyChanged?.Invoke(this,new(nameof(Status))); }
        public void PublishRoute(RoutePlanningState value) { RoutePlanning=value; RoutePlanningChanged?.Invoke(this,value); }
        public void ReportUserError(string message) => LastFault=message;
        public Task SetRouteNameAsync(string name) => Task.CompletedTask;
        public Task LoadRoutesAsync() => Task.CompletedTask;
        public Task LoadRouteAsync(string name) => Task.CompletedTask;
        public Task SetItemsEnabledAsync(IEnumerable<string> ids,bool enabled) => Task.CompletedTask;
        public Task SetDiagnosticsCaptureAsync(bool enabled) => Task.CompletedTask;
        public Task<RoutePlanningState> ExecuteRoutePlanningAsync(string action,object? arguments=null,CancellationToken cancellationToken=default)
        { RouteCommands.Add((action,arguments)); return Task.FromResult(RoutePlanning); }
        public Task<bool> ConfigureAsync(int? captureWay=null,int? mapUpdateCycle=null,int? minMapUpdateCycle=null,bool? mapEnabled=null,bool? minMapEnabled=null,bool? savedPointsEnabled=null,bool? statusBarEnabled=null,CancellationToken cancellationToken=default,int? nearestCompletionKey=null,int? manualRouteKey=null,int? currentTargetGuideKey=null,int? guidePreviousImageKey=null,int? guideNextImageKey=null,bool? gamepadEnabled=null,int? gamepadControllerIndex=null,GamepadButtons? gamepadEntryButton=null,bool? autoReplanEnabled=null)
        {
            if(RejectConfigure) { LastFault="测试保存拒绝"; return Task.FromResult(false); }
            var next=Configuration with {
                CaptureWay=captureWay??Configuration.CaptureWay,MapUpdateCycle=mapUpdateCycle??Configuration.MapUpdateCycle,MinMapUpdateCycle=minMapUpdateCycle??Configuration.MinMapUpdateCycle,
                MapEnabled=mapEnabled??Configuration.MapEnabled,MinMapEnabled=minMapEnabled??Configuration.MinMapEnabled,SavedPointsEnabled=savedPointsEnabled??Configuration.SavedPointsEnabled,StatusBarEnabled=statusBarEnabled??Configuration.StatusBarEnabled,
                NearestCompletionKey=nearestCompletionKey??Configuration.NearestCompletionKey,ManualRouteKey=manualRouteKey??Configuration.ManualRouteKey,CurrentTargetGuideKey=currentTargetGuideKey??Configuration.CurrentTargetGuideKey,GuidePreviousImageKey=guidePreviousImageKey??Configuration.GuidePreviousImageKey,GuideNextImageKey=guideNextImageKey??Configuration.GuideNextImageKey,
                GamepadEnabled=gamepadEnabled??Configuration.GamepadEnabled,GamepadControllerIndex=gamepadControllerIndex??Configuration.GamepadControllerIndex,AutoReplanEnabled=autoReplanEnabled??Configuration.AutoReplanEnabled };
            next.Validate();Configuration=next;PropertyChanged?.Invoke(this,new(nameof(Configuration)));
            PublishRoute(RoutePlanning with { AutoReplanEnabled=next.AutoReplanEnabled });
            return Task.FromResult(true);
        }
    }
}
