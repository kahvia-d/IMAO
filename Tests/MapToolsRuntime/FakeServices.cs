using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text.Json;
using IMao_WinUI.Models;

namespace IMao_WinUI
{
    public static class App
    {
        public static Dictionary<Type, object> Services { get; } = new();
        public static T GetService<T>() where T : class => (T)Services[typeof(T)];
    }
}
namespace IMao_WinUI.Helpers
{
    public sealed record FilterItemDatas(string Name, int Status);
    // Production service's default constructor is linked for compilation but
    // tests exclusively use its injected catalog/persistence constructor.
    public sealed class LocalItemFilter
    {
        public string LastError => "";
        public List<FilterItemDatas> GetFilteredItemsDatas() => throw new InvalidOperationException("Use injected fixture storage");
        public bool SetItemsStatus(IEnumerable<string> ids, int status) => throw new InvalidOperationException("No user filter writes in harness");
    }
}
namespace IMao_WinUI.StringItems
{
    public sealed record FixtureItem(string Id, string Name_SpecifiedLanguage);
    public sealed record FixtureGroup(string Category, List<FixtureItem> ItemDatas);
    public sealed class StringItem
    {
        public List<FixtureGroup> itemsDatas { get; } = new();
        public void LoadString(string language) => throw new InvalidOperationException("Use fixture catalog");
    }
}
namespace IMao_WinUI.Services
{
    internal sealed class ReplyGate
    {
        private readonly TaskCompletionSource entered = new(TaskCreationOptions.RunContinuationsAsynchronously);
        private readonly TaskCompletionSource released = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public Task Entered => entered.Task;
        public void Release() => released.TrySetResult();
        // Deliberately model a committed request whose reply can arrive after cancellation.
        public async Task WaitAsync() { entered.TrySetResult(); await released.Task; }
    }
    // Guide opening is outside this harness; it does not replace the tools,
    // animation, controller interpreter, focus activation or return code.
    public sealed class GamepadHandoffLease(Action<nint, bool> finished) : IDisposable
    {
        private bool done;
        public void Complete(nint target) { if (!done) { done = true; finished(target, true); } }
        public void Dispose() { if (!done) { done = true; finished(0, false); } }
    }
    public sealed class MarkerGuideCoordinator
    {
        public Task OpenRouteGuideFromToolsAsync(string profile, nint game, nint source) => Task.CompletedTask;
    }
    public sealed class CoreHostService : INotifyPropertyChanged
    {
        public event PropertyChangedEventHandler? PropertyChanged;
        public event EventHandler<JsonElement>? MarkerEvent;
        public event EventHandler<RoutePlanningState>? RoutePlanningChanged;
        public RuntimeConfiguration Configuration { get; private set; } = new();
        public CoreRuntimeStatus Status { get; private set; } = new() { CoreState = "running", GameState = "bigMap" };
        public RoutePlanningState RoutePlanning { get; private set; } = new()
        {
            ProfileId = "fixture", SceneId = 1, SceneName = "World", Generation = 4, Revision = 6,
            Active = new() { Id = "route-fixture", Name = "测试采集路线", SceneId = 1, Stops = [new() { Key = "1:a", Name = "声匣", PointId = "a" }] },
            CurrentTarget = new() { Key = "1:a", Name = "声匣", PointId = "a" }, NavigationStatus = "navigating"
        };
        public bool IsConnected { get; private set; } = true;
        public string LastFault => "";
        public nint Game { get; set; }
        public List<(string Type, JsonElement Data)> Commands { get; } = [];
        public List<string> Diagnostics { get; } = [];
        public ulong Session { get; private set; }
        internal ReplyGate? RegisterDelay, UnregisterDelay, RouteDelay, CanvasEnableDelay;
        private ulong nextSession, resultRevision;
        private nint host;
        private string canvas = "pan", page = "home";
        private uint lastButtons;
        public JsonElement Context => JsonSerializer.SerializeToElement(new { gameHwnd = (ulong)Game, profileId = "fixture", contextGeneration = 12UL });
        public void ReportGamepadDiagnostic(string tag, string message)
        { Diagnostics.Add(tag + ": " + message); File.AppendAllText(Path.Combine(AppContext.BaseDirectory, "diagnostics.log"), tag + ": " + message + "\n"); }
        public Task<bool> SynchronizeFilterAsync(IReadOnlyDictionary<string, bool> values, CancellationToken cancellationToken = default) => Task.FromResult(IsConnected);
        public Task<bool> ConfigureAsync(bool? autoReplanEnabled = null, string? expectedAutoReplanProfile = null, CancellationToken cancellationToken = default)
        {
            Configuration = Configuration with { AutoReplanEnabled = autoReplanEnabled ?? Configuration.AutoReplanEnabled };
            RoutePlanning = RoutePlanning with { AutoReplanEnabled = Configuration.AutoReplanEnabled };
            PropertyChanged?.Invoke(this, new(nameof(Configuration))); RoutePlanningChanged?.Invoke(this, RoutePlanning); return Task.FromResult(true);
        }
        public async Task<RoutePlanningState> ExecuteRoutePlanningAsync(string action, object arguments, CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var data = JsonSerializer.SerializeToElement(arguments); Commands.Add(("route:" + action, data));
            if (RouteDelay is { } delayed) { RouteDelay = null; await delayed.WaitAsync(); }
            RoutePlanning = action switch
            {
                "new" => RoutePlanning with { Enabled = true, Tool = "pan", SelectedCount = 3, Start = new() { Valid = true, SceneId = 1 } },
                "tool" => RoutePlanning with { Enabled = true, Tool = data.GetProperty("tool").GetString()! },
                "end" => RoutePlanning with { Enabled = false, Tool = "pan" },
                "clear" => RoutePlanning with { SelectedCount = 0 },
                _ => RoutePlanning
            };
            RoutePlanning = RoutePlanning with { Revision = RoutePlanning.Revision + 1 };
            RoutePlanningChanged?.Invoke(this, RoutePlanning); return RoutePlanning;
        }
        public async Task<JsonElement> ExecuteConnectedMarkerAsync(string type, object arguments, CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (!IsConnected) throw new InvalidOperationException("Core disconnected; no implicit restart permitted");
            var data = JsonSerializer.SerializeToElement(arguments); Commands.Add((type, data));
            switch (type)
            {
                case "markerMapToolsRegister":
                    if (GetForegroundWindow() != Game || data.GetProperty("gameHwnd").GetUInt64() != (ulong)Game)
                        throw new InvalidOperationException("Register must precede activation from actual game foreground");
                    host = (nint)data.GetProperty("hostHwnd").GetUInt64();
                    GetWindowThreadProcessId(host, out uint owner);
                    if (owner != Environment.ProcessId) throw new InvalidOperationException("Host is not owned by harness process");
                    Session = ++nextSession; resultRevision = 0; lastButtons = 0; canvas = "pan"; page = "home";
                    var registered = State();
                    if (RegisterDelay is { } registration) { RegisterDelay = null; await registration.WaitAsync(); }
                    return registered;
                case "markerMapToolsUpdate":
                    RequireSession(data);
                    if (data.GetProperty("interactive").GetBoolean() && data.GetProperty("canvasTool").GetString() != "pan" && CanvasEnableDelay is { } enabling)
                    { CanvasEnableDelay = null; await enabling.WaitAsync(); RequireSession(data); }
                    page = data.GetProperty("page").GetString()!;
                    canvas = data.GetProperty("expectedResultRevision").GetUInt64() == resultRevision ? data.GetProperty("canvasTool").GetString()! : "pan";
                    break;
                case "markerMapToolsInput":
                    RequireSession(data);
                    if (GetForegroundWindow() != host) throw new InvalidOperationException("Canvas input without tools foreground");
                    uint buttons = data.GetProperty("buttons").GetUInt32();
                    if (canvas != "pan" && (lastButtons & 0x1000) != 0 && (buttons & 0x1000) == 0)
                    { canvas = "pan"; ++resultRevision; }
                    lastButtons = buttons; break;
                case "markerMapToolsUnregister":
                    if (UnregisterDelay is { } retirement) { UnregisterDelay = null; await retirement.WaitAsync(); }
                    if (data.GetProperty("sessionId").GetUInt64() == Session) Session = 0; break;
                default: throw new InvalidOperationException("Unexpected fixture IPC " + type);
            }
            return State();
        }
        public void FinishCanvas()
        {
            canvas = "pan"; ++resultRevision;
            MarkerEvent?.Invoke(this, JsonSerializer.SerializeToElement(new { type = "markerMapToolsCanvasChanged", data = State() }));
        }
        public void SetConnected(bool connected)
        {
            IsConnected = connected; Status = new() { CoreState = connected ? "running" : "stopped" };
            PropertyChanged?.Invoke(this, new(nameof(IsConnected)));
        }
        private void RequireSession(JsonElement data)
        { if (Session == 0 || data.GetProperty("sessionId").GetUInt64() != Session) throw new InvalidOperationException("Stale session"); }
        private JsonElement State() => JsonSerializer.SerializeToElement(new
        { sessionId = Session, phase = Session == 0 ? "ended" : canvas == "pan" ? "panel" : "canvas", canvasTool = canvas, page, resultRevision, drawing = false, message = "", hostHwnd = (ulong)host, gameHwnd = (ulong)Game });
        [DllImport("user32.dll")] private static extern nint GetForegroundWindow();
        [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(nint window, out uint process);
    }
}
