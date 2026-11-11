using IMao_WinUI.Helpers;
using IMao_WinUI.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Windowing;
using System.Runtime.InteropServices;
using Windows.Graphics;
using Windows.UI.ViewManagement;

namespace IMao_WinUI;

public sealed partial class MainWindow : WindowEx
{
    private Microsoft.UI.Dispatching.DispatcherQueue dispatcherQueue;
    private UISettings settings;
    private readonly MainWindowPlacementStore _placementStore = new(UserDataPaths.Root, StartupLog.Write);
    private readonly MainWindowPlacementTracker _placement = new();

    public MainWindow()
    {
        StartupLog.Write("main-window-ctor");
        InitializeComponent();
        this.Closed += MainWindow_Closed;
        AppWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets/WindowIcon.ico"));
        Content = null;
        Title = "AppDisplayName".GetLocalized();
        // Apply before Activate/first paint, and do not let WindowEx create its own persistence file.
        RestorePlacement();
        AppWindow.Changed += MainWindow_AppWindowChanged;
        AppWindow.Closing += (_, _) => RememberNormalBounds();

        dispatcherQueue = Microsoft.UI.Dispatching.DispatcherQueue.GetForCurrentThread();
        settings = new UISettings();
        settings.ColorValuesChanged += Settings_ColorValuesChanged;
    }

    private void MainWindow_Closed(object sender, WindowEventArgs args)
    {
        // Closing can be canceled by graceful shutdown. Only a completed close commits the snapshot.
        AppWindow.Changed -= MainWindow_AppWindowChanged;
        if (_placement.LastNormal is { } placement) _placementStore.Save(placement);
        settings.ColorValuesChanged -= Settings_ColorValuesChanged;
        if (!App.RuntimeServicesInitialized) return;
        App.GetService<MarkerGuideCoordinator>().Dispose();
        _ = App.GetService<CoreHostService>().ShutdownAsync();
    }

    private void RestorePlacement()
    {
        var saved = _placementStore.Load();
        var area = saved is null
            ? DisplayArea.GetFromWindowId(AppWindow.Id, DisplayAreaFallback.Primary)
            : DisplayArea.GetFromRect(ToRect(saved.Bounds), DisplayAreaFallback.Nearest);
        if (area is null || area.WorkArea.Width <= 0 || area.WorkArea.Height <= 0)
        {
            StartupLog.Write("main-window-placement-no-work-area");
            return;
        }
        var work = area.WorkArea;
        // Move the still-hidden window to the selected display first. GetDpiForWindow then supplies
        // its authoritative per-monitor DPI (including mixed-DPI and negative-coordinate displays).
        AppWindow.Move(new PointInt32(work.X, work.Y));
        double dpi = GetDpiForWindow(WinRT.Interop.WindowNative.GetWindowHandle(this));
        if (dpi == 0) { StartupLog.Write("main-window-placement-dpi-unavailable"); dpi = 96; }
        MinWidth = Math.Min(800, work.Width * 96d / dpi);
        MinHeight = Math.Min(500, work.Height * 96d / dpi);
        var restored = MainWindowPlacementStore.Restore(saved, new(work.X, work.Y, work.Width, work.Height), dpi);
        AppWindow.MoveAndResize(ToRect(restored));
        RememberNormalBounds();
    }

    private void MainWindow_AppWindowChanged(AppWindow sender, AppWindowChangedEventArgs args)
    {
        if (args.DidPositionChange || args.DidSizeChange || args.DidPresenterChange) RememberNormalBounds();
    }

    private void RememberNormalBounds()
    {
        IntPtr handle = WinRT.Interop.WindowNative.GetWindowHandle(this);
        bool notRestored = IsIconic(handle) || IsZoomed(handle) ||
            AppWindow.Presenter is not OverlappedPresenter { State: OverlappedPresenterState.Restored };
        if (notRestored) return;
        uint dpi = GetDpiForWindow(handle);
        if (dpi == 0) { StartupLog.Write("main-window-placement-capture-dpi-unavailable"); return; }
        _placement.Observe(new(AppWindow.Position.X, AppWindow.Position.Y, AppWindow.Size.Width, AppWindow.Size.Height), dpi, false);
    }

    private static RectInt32 ToRect(PlacementBounds b) => new((int)b.X, (int)b.Y, (int)b.Width, (int)b.Height);

    [DllImport("user32.dll")]
    private static extern uint GetDpiForWindow(IntPtr window);
    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsZoomed(IntPtr window);

    private void Settings_ColorValuesChanged(UISettings sender, object args)
    {
        dispatcherQueue.TryEnqueue(() => TitleBarHelper.ApplySystemThemeToCaptionButtons());
    }
}
