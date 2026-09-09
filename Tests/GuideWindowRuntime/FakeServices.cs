using IMao_WinUI.Models;
using System.ComponentModel;
using System.Text.Json;

namespace IMao_WinUI.Services;

internal sealed record RecordedCommand(string Operation, JsonElement Arguments);

internal sealed class DeferredCommand
{
    public TaskCompletionSource<JsonElement> Seen { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
    public TaskCompletionSource<JsonElement> Reply { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
}

// Only the methods used by the production coordinator are present. No process, pipe, storage or network exists here.
public sealed class CoreHostService : INotifyPropertyChanged
{
    private readonly Dictionary<string, Queue<DeferredCommand>> deferred = new();
    private bool connected = true;
    public event EventHandler<JsonElement>? MarkerEvent;
    public event PropertyChangedEventHandler? PropertyChanged;
    public bool IsConnected
    {
        get => connected;
        set { connected = value; PropertyChanged?.Invoke(this, new(nameof(IsConnected))); }
    }
    public RoutePlanningState RoutePlanning { get; set; } = new();
    public CoreRuntimeStatus Status { get; set; } = new() { CoreState = "running" };
    private RuntimeConfiguration configuration = new();
    public RuntimeConfiguration Configuration
    {
        get => configuration;
        set { configuration = value; PropertyChanged?.Invoke(this, new(nameof(Configuration))); }
    }
    public JsonElement GameWindowBounds { get; set; } = JsonSerializer.SerializeToElement(new { available = false });
    public JsonElement GamepadTargets { get; set; } = JsonSerializer.SerializeToElement(new { });
    public JsonElement GamepadContext { get; set; } = JsonSerializer.SerializeToElement(new { });
    public MarkerSelection? AuthoritativeTarget { get; set; }
    public string ActiveProfile { get; set; } = "local";
    public string ActiveRouteId { get; set; } = "test-route";
    public ulong AuthoritativeRevision { get; set; } = 1;
    public JsonElement RegisteredGuide { get; private set; } = JsonSerializer.SerializeToElement(new { hwnd = 0L });
    internal List<RecordedCommand> Commands { get; } = [];
    internal List<string> Errors { get; } = [];
    internal List<string> GamepadDiagnostics { get; } = [];
    internal Func<string, JsonElement, JsonElement>? RouteGamepadResponder { get; set; }
    internal Func<string, JsonElement, JsonElement>? NearbyResponder { get; set; }
    internal Func<string, JsonElement, JsonElement>? CursorResponder { get; set; }

    internal DeferredCommand DeferNext(string operation)
    {
        if (!deferred.TryGetValue(operation, out var queue)) deferred[operation] = queue = new();
        var pending = new DeferredCommand(); queue.Enqueue(pending); return pending;
    }

    public async Task<JsonElement> ExecuteMarkerAsync(string operation, object arguments, CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var command = JsonSerializer.SerializeToElement(arguments);
        Commands.Add(new(operation, command));
        JsonElement response;
        if (deferred.TryGetValue(operation, out var queue) && queue.Count > 0)
        {
            var pending = queue.Dequeue();
            pending.Seen.TrySetResult(command);
            // Deliberately allow a late reply after cancellation: production generation guards must still reject it.
            response = await pending.Reply.Task;
        }
        else if (operation == "markerRouteGamepadReturnStatus")
        {
            if (command.GetProperty("sessionId").GetUInt64() == 0 ||
                command.GetProperty("status").GetString() is not ("returning" or "failed"))
                throw new InvalidOperationException("Invalid test return display status");
            response = JsonSerializer.SerializeToElement(new { accepted = true });
        }
        else if (operation.StartsWith("markerRouteGamepad", StringComparison.Ordinal) && RouteGamepadResponder is { } responder)
            response = responder(operation, command);
        else if (operation is "markerGetNearbyGuide" or "markerBindNearbyCandidates" or "markerResolveNearbyCandidate" or "markerCompleteNearbyCandidate" && NearbyResponder is { } nearbyResponder)
            response = nearbyResponder(operation, command);
        else if (operation == "markerResolveGamepadCursorCandidate" && CursorResponder is { } cursorResponder)
            response = cursorResponder(operation, command);
        else response = operation switch
        {
            "markerGetRouteGuide" => RouteGuideResponse(),
            "markerGetGameWindowBounds" => GameWindowBounds,
            "markerGetGamepadTargets" => GamepadTargets,
            "markerGetGamepadContext" => GamepadContext,
            "markerSetGuideWindow" => Empty(),
            "markerSetCompletion" => JsonSerializer.SerializeToElement(new { point = command }),
            "markerGetSnapshot" => JsonSerializer.SerializeToElement(new { points = Array.Empty<object>() }),
            _ => throw new InvalidOperationException("Unexpected test command: " + operation)
        };
        if (operation == "markerSetGuideWindow") RegisteredGuide = command;
        return response;
    }

    public async Task<RoutePlanningState> ExecuteRoutePlanningAsync(string action, object? arguments = null,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        string operation = "routePlanning:" + action;
        var command = JsonSerializer.SerializeToElement(arguments ?? new { });
        Commands.Add(new(operation, command));
        if (deferred.TryGetValue(operation, out var queue) && queue.Count > 0)
        {
            var pending = queue.Dequeue();
            pending.Seen.TrySetResult(command);
            return RoutePlanningState.FromJson(await pending.Reply.Task);
        }
        if (action == "state") return RoutePlanning;
        // Mutations require an explicit controlled reply; no fake silent success.
        throw new InvalidOperationException("Unexpected test route command: " + action);
    }

    public void ReportUserError(string message) => Errors.Add(message);
    public void ReportGamepadDiagnostic(string category, string message) => GamepadDiagnostics.Add(category + ": " + message);
    internal void Emit(object value) => MarkerEvent?.Invoke(this, JsonSerializer.SerializeToElement(value));
    internal JsonElement RouteGuideResponse() => JsonSerializer.SerializeToElement(new
    {
        profileId = ActiveProfile, routeId = ActiveRouteId, revision = AuthoritativeRevision,
        selection = AuthoritativeTarget is { } target ? SelectionPayload(target) : null
    });
    internal static object SelectionPayload(MarkerSelection point) => new
    {
        type = "markerSelected", profileId = point.ProfileId, sceneName = point.Scene, nameId = point.NameId,
        pointId = point.PointId, stateId = point.StateId, countryId = point.CountryId, floorId = point.FloorId,
        level = point.Level, completed = point.Completed, screenX = point.ScreenX, screenY = point.ScreenY
    };
    internal static JsonElement Empty() => JsonSerializer.SerializeToElement(new { });
}

public sealed class MarkerDetailService
{
    internal string[] Pictures { get; set; } = [];
    internal List<string> PictureRequests { get; } = [];
    internal HashSet<string> FailedPictures { get; } = [];
    internal Queue<TaskCompletionSource<MarkerDetail>> LocalReplies { get; } = new();
    internal Queue<TaskCompletionSource<MarkerDetailResult>> OnlineReplies { get; } = new();
    internal List<MarkerSelection> LocalRequests { get; } = [];
    internal List<MarkerSelection> OnlineRequests { get; } = [];
    public Task<MarkerDetail> GetLocalAsync(MarkerSelection selection, CancellationToken cancellationToken = default)
    {
        LocalRequests.Add(selection);
        return LocalReplies.Count > 0 ? LocalReplies.Dequeue().Task : Task.FromResult(Detail(selection) with { PictureUrls = Pictures });
    }
    public Task<MarkerDetailResult> GetOnlineAsync(MarkerSelection selection, MarkerDetail local,
        CancellationToken cancellationToken = default, bool refresh = false)
    {
        OnlineRequests.Add(selection);
        return OnlineReplies.Count > 0 ? OnlineReplies.Dequeue().Task : Task.FromResult(new MarkerDetailResult(local, "Test guide ready"));
    }
    public async Task<string> GetPicturePathAsync(string url, CancellationToken cancellationToken = default)
    {
        if (!Pictures.Contains(url)) throw new InvalidOperationException("Unexpected image URL; test never accesses the network");
        PictureRequests.Add(url);
        if (FailedPictures.Contains(url)) throw new IOException("Controlled image load failure");
        string folderPath = Path.Combine(AppContext.BaseDirectory, "test-pictures");
        Directory.CreateDirectory(folderPath);
        int index = Array.IndexOf(Pictures, url);
        string name = "controlled-page-" + index + ".png";
        string path = Path.Combine(folderPath, name);
        if (File.Exists(path)) return path;
        var folder = await Windows.Storage.StorageFolder.GetFolderFromPathAsync(folderPath);
        var file = await folder.CreateFileAsync(name, Windows.Storage.CreationCollisionOption.ReplaceExisting);
        using var stream = await file.OpenAsync(Windows.Storage.FileAccessMode.ReadWrite);
        var encoder = await Windows.Graphics.Imaging.BitmapEncoder.CreateAsync(Windows.Graphics.Imaging.BitmapEncoder.PngEncoderId, stream);
        byte[] pixels = new byte[256 * 128 * 4];
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 256; ++x)
            {
                int offset = (y * 256 + x) * 4;
                pixels[offset] = (byte)(index == 0 ? 50 + y : 220);
                pixels[offset + 1] = (byte)(60 + x / 2);
                pixels[offset + 2] = (byte)(index == 0 ? 220 : 50 + y);
                pixels[offset + 3] = 255;
            }
        encoder.SetPixelData(Windows.Graphics.Imaging.BitmapPixelFormat.Bgra8, Windows.Graphics.Imaging.BitmapAlphaMode.Ignore,
            256, 128, 96, 96, pixels);
        await encoder.FlushAsync();
        return path;
    }
    public static bool TryGetExternalUri(string? value, out Uri uri) { uri = null!; return false; }
    internal static MarkerDetail Detail(MarkerSelection selection) => new()
    {
        PointId = selection.PointId, StateId = selection.StateId, CountryId = selection.CountryId,
        Name = "Test guide " + selection.PointId, Description = "Controlled local text; no network or user data.",
        FloorId = selection.FloorId, Level = selection.Level
    };
}
