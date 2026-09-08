using IMao_WinUI.Models;
using IMao_WinUI.Views;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using System.ComponentModel;
using System.Text.Json;
using Windows.Graphics;

namespace IMao_WinUI.Services;

public sealed class MarkerGuideCoordinator : IDisposable
{
    private readonly CoreHostService core;
    private readonly MarkerDetailService details;
    private MarkerGuideWindow? guide;
    private Window? chooser;
    private readonly MarkerGuideSession session = new();
    private readonly SemaphoreSlim registrationLock = new(1, 1);
    private CancellationTokenSource connectionRequests = new();
    private CancellationTokenSource? openingRequest;
    private Window? registeredWindow;
    private object registration = new { hwnd = 0L };
    private long registrationRevision;
    private long publishedRegistrationRevision;
    private bool disposed;
    private long selectionGeneration;
    private long completionGeneration;

    public MarkerGuideCoordinator(CoreHostService core, MarkerDetailService details)
    {
        this.core = core;
        this.details = details;
        core.MarkerEvent += OnMarkerEvent;
        core.PropertyChanged += OnCorePropertyChanged;
    }

    private void OnCorePropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (disposed) return;
        if (e.PropertyName == nameof(CoreHostService.Configuration))
        {
            var configuration = core.Configuration;
            guide?.SetPagingHotkeys(configuration.GuidePreviousImageKey, configuration.GuideNextImageKey);
            return;
        }
        if (e.PropertyName != nameof(CoreHostService.IsConnected) || core.IsConnected) return;
        // A successor CoreHost has no registered HWND. Close locally without starting it just to unregister.
        registeredWindow = null;
        registration = new { hwnd = 0L };
        registrationRevision++;
        CancelOpeningRequest();
        connectionRequests.Cancel();
        connectionRequests.Dispose();
        connectionRequests = new();
        session.Close();
        selectionGeneration++;
        guide?.HideGuide();
        chooser?.Close(); chooser = null;
    }

    private async void OnMarkerEvent(object? sender, JsonElement value)
    {
        if (disposed) return;
        try
        {
            switch (value.GetProperty("type").GetString())
            {
                case "markerSelected": await ShowAsync(ReadSelection(value)); break;
                case "markerCandidates": await ShowCandidatesAsync(value); break;
                case "markerGuideShortcut": await ToggleGuideAsync(value); break;
                case "markerGuideCompleteRequested":
                    if (IsCurrentGuideEvent(value)) await guide!.CompleteCurrentAsync();
                    break;
                case "markerGuidePageRequested":
                    if (IsCurrentGuideEvent(value)) await guide!.ChangePictureAsync(Integer(value, "direction"));
                    break;
                case "markerCompletionChanged":
                    long completionVersion = ++completionGeneration;
                    var point = value.TryGetProperty("point", out var nested) ? nested : value;
                    if (point.TryGetProperty("pointId", out var id) && point.TryGetProperty("completed", out var completed))
                        ApplyCompletionEvent(value, Integer(point, "stateId"), id.GetString() ?? "", completed.GetBoolean());
                    else if (session.Selection is { } selection && selection.ProfileId == Text(value, "profileId"))
                    {
                        long generation = session.Generation;
                        var snapshot = await core.ExecuteMarkerAsync("markerGetSnapshot", new
                        { profileId = selection.ProfileId, stateId = selection.StateId, pointId = selection.PointId, limit = 1 }, connectionRequests.Token);
                        if (disposed || !session.IsCurrent(generation) || completionVersion != completionGeneration) break;
                        bool nowCompleted = snapshot.GetProperty("points").EnumerateArray()
                            .Any(p => p.GetProperty("completed").GetBoolean());
                        ApplyCompletionEvent(value, selection.StateId, selection.PointId, nowCompleted);
                    }
                    break;
                case "markerProfileChanged":
                case "markerSelectionCleared":
                    CloseGuide();
                    selectionGeneration++;
                    chooser?.Close(); chooser = null;
                    break;
            }
        }
        catch (Exception e) when (e is IOException or InvalidOperationException or JsonException or ArgumentException or OperationCanceledException)
        { core.ReportUserError("无法显示点位攻略：" + e.Message); }
    }

    internal static MarkerSelection ReadSelection(JsonElement value) => new()
    {
        ProfileId = value.TryGetProperty("profileId", out var profile) ? profile.GetString() ?? "local" : "local",
        Scene = Text(value, "sceneName"), NameId = Text(value, "nameId"), PointId = Text(value, "pointId"),
        StateId = Integer(value, "stateId"), CountryId = Integer(value, "countryId"),
        FloorId = Text(value, "floorId"), Level = Text(value, "level"),
        Completed = value.TryGetProperty("completed", out var completed) && completed.GetBoolean(),
        ScreenX = Number(value, "screenX"), ScreenY = Number(value, "screenY")
    };
    private static string Text(JsonElement value, string key) => value.TryGetProperty(key, out var p) && p.ValueKind == JsonValueKind.String ? p.GetString() ?? "" : "";
    private static int Integer(JsonElement value, string key) => value.TryGetProperty(key, out var p) && p.TryGetInt32(out var n) ? n : 0;
    private static long Long(JsonElement value, string key) => value.TryGetProperty(key, out var p) && p.TryGetInt64(out var n) ? n : 0;
    private static double Number(JsonElement value, string key) => value.TryGetProperty(key, out var p) && p.TryGetDouble(out var n) ? n : 0;

    private bool IsCurrentGuideEvent(JsonElement value) =>
        guide is { IsGuideVisible: true } && session.Selection is { } current &&
        session.IsCurrent(Long(value, "selectionGeneration")) && Long(value, "hwnd") == WindowHandle(guide) &&
        current.ProfileId == Text(value, "profileId") && current.StateId == Integer(value, "stateId") &&
        current.PointId == Text(value, "pointId");

    private async Task ToggleGuideAsync(JsonElement value)
    {
        string profileId = Text(value, "profileId");
        if (session.IsOpen && session.ProfileId != profileId) return;
        if (session.IsOpen) { CloseGuide(); return; }
        if (chooser is not null) { selectionGeneration++; chooser.Close(); chooser = null; return; }
        // The route snapshot has lower IPC priority than completion events. It can still contain
        // the point just completed, so resolve the target in the core with a correlated read.
        CancelOpeningRequest();
        long generation = session.OpenPending(profileId);
        selectionGeneration++;
        var request = CancellationTokenSource.CreateLinkedTokenSource(connectionRequests.Token);
        openingRequest = request;
        try
        {
            while (session.IsCurrent(generation))
            {
                long completionVersion = completionGeneration;
                var result = await core.ExecuteMarkerAsync("markerGetRouteGuide", new
                { profileId, screenX = Number(value, "screenX"), screenY = Number(value, "screenY") }, request.Token);
                if (!session.IsCurrent(generation) || disposed) return;
                // A completion observed while awaiting the response invalidates that target snapshot.
                if (completionVersion != completionGeneration) continue;
                if (Text(result, "profileId") != profileId || !result.TryGetProperty("selection", out var target) ||
                    target.ValueKind == JsonValueKind.Null) { CloseGuide(generation); return; }
                var selection = ReadSelection(target);
                if (selection.Completed || selection.StateId <= 0 || selection.PointId.Length == 0 ||
                    !session.SetSelection(generation, selection)) { CloseGuide(generation); return; }
                if (await ShowCurrentAsync(selection, generation, completionVersion)) return;
            }
        }
        catch (OperationCanceledException) when (request.IsCancellationRequested) { }
        catch { CloseGuide(generation); throw; }
        finally
        {
            if (ReferenceEquals(openingRequest, request)) openingRequest = null;
            request.Dispose();
        }
    }

    public async Task ShowAsync(MarkerSelection selection)
    {
        if (disposed) return;
        CancelOpeningRequest();
        selectionGeneration++;
        chooser?.Close(); chooser = null;
        long generation = session.Open(selection);
        await ShowCurrentAsync(selection, generation);
    }

    private async Task<bool> ShowCurrentAsync(MarkerSelection selection, long generation, long? routeCompletionVersion = null)
    {
        // Never register B while A is still visible and could receive its completion shortcut.
        guide?.HideGuide();
        if (guide is null || guide.IsClosed)
        {
            guide = new MarkerGuideWindow(details, SetCompletionAsync, dismissedGeneration => CloseGuide(dismissedGeneration));
            var window = guide;
            window.Closed += async (_, _) =>
            {
                if (ReferenceEquals(guide, window))
                {
                    guide = null;
                }
                await UnregisterWindowAsync(window);
            };
        }
        var current = guide;
        try
        {
            var bounds = await core.ExecuteMarkerAsync("markerGetGameWindowBounds", new { }, connectionRequests.Token);
            if (!session.IsCurrent(generation) || disposed) return false;
            RectInt32? gameBounds = null;
            if (bounds.TryGetProperty("available", out var available) && available.ValueKind == JsonValueKind.True)
            {
                int left = Integer(bounds, "left"), top = Integer(bounds, "top");
                int right = Integer(bounds, "right"), bottom = Integer(bounds, "bottom");
                long width = (long)right - left, height = (long)bottom - top;
                if (width is > 0 and <= 100000 && height is > 0 and <= 100000)
                    gameBounds = new RectInt32(left, top, (int)width, (int)height);
            }
            var configuration = core.Configuration;
            current.SetPagingHotkeys(configuration.GuidePreviousImageKey, configuration.GuideNextImageKey);
            await RegisterWindowAsync(current, selection, generation);
            if (!session.IsCurrent(generation) || disposed) return false;
            // Registration is another IPC await. A synchronized completion may update A
            // without closing this session, so a route shortcut must resolve again before activation.
            if (routeCompletionVersion is { } version &&
                (version != completionGeneration || session.Selection?.Completed != false)) return false;
            await current.ShowMarkerAsync(session.Selection!, generation, gameBounds);
            return true;
        }
        catch { CloseGuide(generation); throw; }
    }

    private static long WindowHandle(Window window) => WinRT.Interop.WindowNative.GetWindowHandle(window).ToInt64();

    private Task RegisterWindowAsync(Window window, MarkerSelection? selection = null, long generation = 0)
    {
        registeredWindow = window;
        registration = selection is null ? new { hwnd = WindowHandle(window) } : (object)new
        {
            hwnd = WindowHandle(window), profileId = selection.ProfileId, stateId = selection.StateId,
            pointId = selection.PointId, selectionGeneration = generation
        };
        registrationRevision++;
        return PublishRegistrationAsync();
    }

    private async Task PublishRegistrationAsync()
    {
        await registrationLock.WaitAsync();
        try
        {
            // An old register/unregister acknowledgement must never become the final native state.
            while (!disposed && core.IsConnected && publishedRegistrationRevision != registrationRevision)
            {
                long revision = registrationRevision;
                object desired = registration;
                try { await core.ExecuteMarkerAsync("markerSetGuideWindow", desired, connectionRequests.Token); }
                catch when (revision != registrationRevision) { continue; }
                publishedRegistrationRevision = revision;
            }
        }
        finally { registrationLock.Release(); }
    }

    private async Task UnregisterWindowAsync(Window window)
    {
        if (!ReferenceEquals(registeredWindow, window)) return;
        registeredWindow = null;
        registration = new { hwnd = 0L };
        registrationRevision++;
        try { await PublishRegistrationAsync(); }
        catch (Exception e) { if (!disposed) core.ReportUserError("攻略窗口快捷键状态未更新：" + e.Message); }
    }

    private void CloseGuide(long? generation = null)
    {
        if (!session.Close(generation)) return;
        HideClosedGuide();
    }

    private void HideClosedGuide()
    {
        CancelOpeningRequest();
        selectionGeneration++;
        if (guide is not { } window) return;
        window.HideGuide();
        _ = UnregisterWindowAsync(window);
    }

    private void CancelOpeningRequest()
    {
        var request = openingRequest;
        openingRequest = null;
        request?.Cancel();
        // Its awaiting operation owns disposal.
    }

    private void ApplyCompletion(long generation, string profileId, int stateId, string pointId, bool completed, bool closeOnComplete)
    {
        var result = session.ApplyCompletion(generation, profileId, stateId, pointId, completed, closeOnComplete: closeOnComplete);
        if (result == MarkerGuideCompletionResult.Closed) HideClosedGuide();
        else if (result == MarkerGuideCompletionResult.Updated && session.Selection is { } selection)
            guide?.UpdateCompletion(selection, generation);
    }

    private void ApplyCompletionEvent(JsonElement value, int stateId, string pointId, bool completed)
    {
        bool close = Text(value, "source") == "local";
        if (value.TryGetProperty("guideSelectionGeneration", out _))
            close &= session.IsCurrent(Long(value, "guideSelectionGeneration")) && guide is not null &&
                WindowHandle(guide) == Long(value, "guideWindowHwnd");
        ApplyCompletion(session.Generation, Text(value, "profileId"), stateId, pointId, completed, close);
    }

    private async Task<bool> SetCompletionAsync(MarkerSelection selection, bool completed, long generation)
    {
        if (!session.IsCurrent(generation) || guide is null || session.Selection is not { } current ||
            current.ProfileId != selection.ProfileId || current.StateId != selection.StateId || current.PointId != selection.PointId) return false;
        long hwnd = WindowHandle(guide);
        try
        {
            await core.ExecuteMarkerAsync("markerSetCompletion", new
            {
                profileId = selection.ProfileId, sceneName = selection.Scene, nameId = selection.NameId, pointId = selection.PointId,
                stateId = selection.StateId, completed, guideSelectionGeneration = generation, guideWindowHwnd = hwnd
            }, connectionRequests.Token);
            ApplyCompletion(generation, selection.ProfileId, selection.StateId, selection.PointId, completed, true);
            return true;
        }
        catch (Exception e) when (e is IOException or InvalidOperationException or OperationCanceledException)
        { if (session.IsCurrent(generation)) core.ReportUserError("点位进度未保存：" + e.Message); return false; }
    }

    private async Task ShowCandidatesAsync(JsonElement page)
    {
        if (page.GetProperty("candidates").GetArrayLength() == 0) return;
        CloseGuide();
        long generation = ++selectionGeneration;
        string profileId = Text(page, "profileId");
        long revision = page.GetProperty("selectionRevision").GetInt64();
        int loaded = 0;
        guide?.HideGuide();
        chooser?.Close();
        var window = new Window { Title = "选择要标记的点位" };
        chooser = window;
        var list = new StackPanel { Spacing = 10, Padding = new Thickness(20) };
        list.Children.Add(new TextBlock { Text = "附近有多个点位，请选择一个查看攻略或标记。", TextWrapping = TextWrapping.Wrap });
        var rows = new StackPanel { Spacing = 8 };
        var more = new Button { Content = "加载更多点位", Visibility = Visibility.Collapsed };
        var notice = new TextBlock { TextWrapping = TextWrapping.Wrap };
        list.Children.Add(rows); list.Children.Add(more); list.Children.Add(notice);
        bool IsCurrent() => generation == selectionGeneration && !disposed && ReferenceEquals(chooser, window);
        async Task AddPageAsync(JsonElement result)
        {
            foreach (var selection in result.GetProperty("candidates").EnumerateArray().Select(ReadSelection))
            {
                if (!IsCurrent()) return;
                var button = new Button { Content = selection.NameId, HorizontalAlignment = HorizontalAlignment.Stretch };
                button.Click += async (_, _) =>
                {
                    if (!IsCurrent()) return;
                    try { window.Close(); chooser = null; await ShowAsync(selection); }
                    catch (Exception) { core.ReportUserError("无法打开所选点位，请重新选择。"); }
                };
                rows.Children.Add(button);
                loaded++;
                var detail = await details.GetLocalAsync(selection);
                if (!IsCurrent()) return;
                button.Content = new TextBlock { Text = $"{detail.Name}  {detail.Level}  · {selection.PointId[^Math.Min(6, selection.PointId.Length)..]}", TextWrapping = TextWrapping.Wrap };
            }
            if (!IsCurrent()) return;
            more.Visibility = result.TryGetProperty("hasMore", out var hasMore) && hasMore.GetBoolean() ? Visibility.Visible : Visibility.Collapsed;
            notice.Text = $"已显示 {loaded} / {Integer(result, "total")} 个点位";
        }
        more.Click += async (_, _) =>
        {
            if (!IsCurrent()) return;
            more.IsEnabled = false;
            try
            {
                var next = await core.ExecuteMarkerAsync("markerGetCandidates", new { profileId, selectionRevision = revision, offset = loaded, limit = 100 }, connectionRequests.Token);
                if (IsCurrent()) await AddPageAsync(next);
            }
            catch (Exception) { if (IsCurrent()) notice.Text = "候选列表已变化或读取失败，请重新选择附近点位。"; }
            finally { if (IsCurrent()) more.IsEnabled = true; }
        };
        window.Content = new ScrollViewer { Content = list };
        window.AppWindow.Resize(new Windows.Graphics.SizeInt32(420, 480));
        window.Closed += async (_, _) => { if (ReferenceEquals(chooser, window)) chooser = null; await UnregisterWindowAsync(window); };
        await RegisterWindowAsync(window);
        if (generation != selectionGeneration || disposed || !ReferenceEquals(chooser, window)) { window.Close(); return; }
        window.Activate();
        await AddPageAsync(page);
    }

    public void Dispose()
    {
        disposed = true;
        CancelOpeningRequest();
        session.Close();
        selectionGeneration++;
        core.MarkerEvent -= OnMarkerEvent;
        core.PropertyChanged -= OnCorePropertyChanged;
        connectionRequests.Cancel();
        connectionRequests.Dispose();
        chooser?.Close(); guide?.Close();
    }
}
