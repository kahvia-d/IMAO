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

    private bool _isFirstActivation = true;
    private UISettings settings;

    public MainWindow()
    {
        InitializeComponent();
        this.Activated += MainWindow_Activated;
        this.Closed += MainWindow_Closed;
        AppWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets/WindowIcon.ico"));
        Content = null;
        Title = "AppDisplayName".GetLocalized();

        // Theme change code picked from https://github.com/microsoft/WinUI-Gallery/pull/1239
        dispatcherQueue = Microsoft.UI.Dispatching.DispatcherQueue.GetForCurrentThread();
        settings = new UISettings();
        settings.ColorValuesChanged += Settings_ColorValuesChanged; // cannot use FrameworkElement.ActualThemeChanged event
    }

    private void MainWindow_Closed(object sender, WindowEventArgs args)
    {
        settings.ColorValuesChanged -= Settings_ColorValuesChanged;
        if (!App.ResourcesInitialized) return;
        App.GetService<MarkerGuideCoordinator>().Dispose();
        _ = App.GetService<CoreHostService>().ShutdownAsync();
    }

    private void MainWindow_Activated(object sender, WindowActivatedEventArgs args)
    {
        if (_isFirstActivation && args.WindowActivationState == WindowActivationState.CodeActivated)
        {
            _isFirstActivation = false;
            FitToCurrentWorkArea();
        }
    }

    private void FitToCurrentWorkArea()
    {
        var area = DisplayArea.GetFromWindowId(AppWindow.Id, DisplayAreaFallback.Nearest);
        if (area is null || area.WorkArea.Width <= 0 || area.WorkArea.Height <= 0) return;
        var work = area.WorkArea;
        uint dpi = GetDpiForWindow(WinRT.Interop.WindowNative.GetWindowHandle(this));
        double scale = dpi == 0 ? 1 : dpi / 96d;
        // WindowEx sizes are logical DIPs; AppWindow and monitor work areas use physical pixels.
        MinWidth = Math.Min(800, work.Width / scale);
        MinHeight = Math.Min(500, work.Height / scale);
        if (AppWindow.Presenter is OverlappedPresenter { State: not OverlappedPresenterState.Restored }) return;
        var current = new RectInt32(AppWindow.Position.X, AppWindow.Position.Y, AppWindow.Size.Width, AppWindow.Size.Height);
        AppWindow.MoveAndResize(MainWindowPlacement.Fit(current, work));
    }

    [DllImport("user32.dll")]
    private static extern uint GetDpiForWindow(IntPtr window);

    // this handles updating the caption button colors correctly when indows system theme is changed
    // while the app is open
    private void Settings_ColorValuesChanged(UISettings sender, object args)
    {
        // This calls comes off-thread, hence we will need to dispatch it to current app's thread
        dispatcherQueue.TryEnqueue(() =>
        {
            TitleBarHelper.ApplySystemThemeToCaptionButtons();
        });
    }
}
