using IMao_WinUI.Activation;
using IMao_WinUI.Contracts.Services;
using IMao_WinUI.Core.Contracts.Services;
using IMao_WinUI.Core.Services;
using IMao_WinUI.Helpers;
using IMao_WinUI.Models;
using IMao_WinUI.Notifications;
using IMao_WinUI.Services;
using IMao_WinUI.ViewModels;
using IMao_WinUI.Views;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.Windows.Globalization;
using System.ComponentModel;
using IMao_WinUI.Core.Updates;
using System.Diagnostics;
using System.Text.Json;

namespace IMao_WinUI;

// To learn more about WinUI 3, see https://docs.microsoft.com/windows/apps/winui/winui3/.
public partial class App : Application
{
    private FileStream? programLease;
    private bool closing, closeApproved;
    public static bool ResourcesInitialized { get; private set; }
    // The .NET Generic Host provides dependency injection, configuration, logging, and other services.
    // https://docs.microsoft.com/dotnet/core/extensions/generic-host
    // https://docs.microsoft.com/dotnet/core/extensions/dependency-injection
    // https://docs.microsoft.com/dotnet/core/extensions/configuration
    // https://docs.microsoft.com/dotnet/core/extensions/logging
    public IHost Host
    {
        get;
    }

    public static T GetService<T>()
        where T : class
    {
        if ((App.Current as App)!.Host.Services.GetService(typeof(T)) is not T service)
        {
            throw new ArgumentException($"{typeof(T)} needs to be registered in ConfigureServices within App.xaml.cs.");
        }

        return service;
    }

    public static WindowEx MainWindow { get; } = new MainWindow();

    public static UIElement? AppTitlebar { get; set; }

    public App()
    {
        InitializeComponent();
        Host = Microsoft.Extensions.Hosting.Host.
        CreateDefaultBuilder().
        UseContentRoot(AppContext.BaseDirectory).
        ConfigureServices((context, services) =>
        {
            // Default Activation Handler
            services.AddTransient<ActivationHandler<LaunchActivatedEventArgs>, DefaultActivationHandler>();

            // Other Activation Handlers
            services.AddTransient<IActivationHandler, AppNotificationActivationHandler>();

            // Services
            services.AddSingleton<IAppNotificationService, AppNotificationService>();
            services.AddSingleton<ILocalSettingsService, LocalSettingsService>();
            services.AddSingleton<IThemeSelectorService, ThemeSelectorService>();
            services.AddSingleton<CoreHostService>();
            services.AddSingleton<KuroProgressSyncService>();
            services.AddSingleton<KuroAutoSyncService>();
            services.AddSingleton<ResourceSnapshotService>(_ => ResourceUpdateBootstrap.CreateSnapshots());
            services.AddSingleton<UpdateService>(provider => ResourceUpdateBootstrap.CreateUpdater(provider.GetRequiredService<ResourceSnapshotService>()));
            services.AddSingleton<UpdateUiController>();
            services.AddSingleton<MarkerDetailService>();
            services.AddSingleton<MarkerGuideCoordinator>();
            services.AddSingleton<FilterSelectionService>();
            services.AddSingleton<MapToolsController>();
            services.AddSingleton<IMapToolsController>(provider => provider.GetRequiredService<MapToolsController>());
            services.AddSingleton<GamepadInputService>();
            services.AddSingleton<ExplorationHotkeyService>();
            services.AddTransient<INavigationViewService, NavigationViewService>();

            services.AddSingleton<IActivationService, ActivationService>();
            services.AddSingleton<IPageService, PageService>();
            services.AddSingleton<INavigationService, NavigationService>();

            // Core Services
            services.AddSingleton<IFileService, FileService>();

            // Views and ViewModels
            services.AddTransient<SettingsViewModel>();
            services.AddTransient<SettingsPage>();
            services.AddTransient<FilterViewModel>();
            services.AddTransient<FilterPage>();
            services.AddTransient<FunctionViewModel>();
            services.AddTransient<FunctionPage>();
            services.AddTransient<StartViewModel>();
            services.AddTransient<StartPage>();
            services.AddTransient<DiagnosticsViewModel>();
            services.AddTransient<DiagnosticsPage>();
            services.AddTransient<UsageGuideViewModel>();
            services.AddTransient<UsageGuidePage>();
            services.AddTransient<ShellPage>();
            services.AddTransient<ShellViewModel>();

            // Configuration
            services.Configure<LocalSettingsOptions>(context.Configuration.GetSection(nameof(LocalSettingsOptions)));
        }).
        Build();

        App.GetService<IAppNotificationService>().Initialize();

        UnhandledException += App_UnhandledException;
    }

    private void App_UnhandledException(object sender, Microsoft.UI.Xaml.UnhandledExceptionEventArgs e)
    {
        // An unhandled XAML exception kills the process with 0xc000027b and leaves nothing behind, which is
        // exactly what makes "it just closes" impossible to diagnose. Record it, then let it fail: silently
        // continuing from a broken visual tree is worse than stopping.
        Trace("unhandled-exception " + e.Message + Environment.NewLine + e.Exception);
    }

    protected async override void OnLaunched(LaunchActivatedEventArgs args)
    {
        var installRoot = ProgramUpdateStore.FindInstallRoot(AppContext.BaseDirectory);
        var launcher = Path.Combine(installRoot, "IMao-Launcher.exe");
        if (File.Exists(launcher) && !ProgramLaunchSession.IsManaged)
        {
            try { Process.Start(new ProcessStartInfo(launcher) { UseShellExecute = true, WorkingDirectory = installRoot }); }
            catch (Exception error) { MainWindow.Content = new TextBlock { Text = "无法启动程序更新器：" + error.Message, Margin = new Thickness(32) }; MainWindow.Activate(); return; }
            MainWindow.Close(); Exit(); return;
        }
        // 强制语言匹配：仅中英，无匹配则用英语
        var excludedLanguagePrefixes = new[] { "en", "zh" };

        var matched = ApplicationLanguages.Languages
            .FirstOrDefault(lang =>
                !excludedLanguagePrefixes.Any(prefix =>
                    lang.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)
                )
            ) ?? "en-us"; // 没有匹配项时默认使用en-us

        base.OnLaunched(args);

        //App.GetService<IAppNotificationService>().Show(string.Format("AppNotificationSamplePayload".GetLocalized(), AppContext.BaseDirectory));

        try
        {
            if (ProgramLaunchSession.IsManaged) programLease = ProgramLauncher.AcquireChildLease(installRoot);
            // This runs before the window is interactive, so it is the one place a slow or blocked step
            // looks like a frozen program. Record how long it takes and what it did, because "the core keeps
            // loading" is otherwise indistinguishable from a hang.
            var startupTimer = System.Diagnostics.Stopwatch.StartNew();
            Trace($"resource-init-start root={installRoot}");
            var snapshots = GetService<ResourceSnapshotService>();
            await snapshots.InitializeAsync();
            ResourceSessionPaths.Initialize(snapshots);
            ResourcesInitialized = true;
            Trace($"resource-init-done elapsedMs={startupTimer.ElapsedMilliseconds} snapshot={snapshots.Current.SnapshotId} "
                + $"packages={snapshots.CurrentRuntimeSnapshot.Packages.Count} pending={snapshots.HasPending} notice={snapshots.LastNotice}");
        }
        catch (Exception error)
        {
            Trace("resource-init-failed " + error);
            MainWindow.Content = new TextBlock { Text = "地图资源初始化失败：" + error.Message + "\n请重新安装完整程序包后再试。", TextWrapping = TextWrapping.Wrap, Margin = new Thickness(32) };
            MainWindow.Activate();
            return;
        }
        _ = GetService<MarkerGuideCoordinator>();
        var mapTools = GetService<MapToolsController>();
        var gamepad = GetService<GamepadInputService>();
        // 键盘侧的启停快捷键必须在**核心已停止**时也活着，所以它住在外壳里（见该服务注释）。
        var explorationHotkey = GetService<ExplorationHotkeyService>();
        MainWindow.Closed += (_, _) => { mapTools.Dispose(); explorationHotkey.Dispose(); gamepad.Dispose(); };
        MainWindow.AppWindow.Closing += async (_, e) =>
        {
            if (closeApproved) return;
            e.Cancel = true;
            await CloseCleanlyAsync();
        };
        var updates = GetService<UpdateUiController>();
        if (ProgramLaunchSession.IsManaged)
        {
            try
            {
                var keys = JsonSerializer.Deserialize<TrustedUpdateKeys>(File.ReadAllBytes(Path.Combine(installRoot, "Assets", "Updates", "trusted-keys.json")), UpdateJson.Options)!;
                updates.AttachProgramUpdater(new ProgramUpdateStore(installRoot, keys.Keys, ResourceUpdateBootstrap.ReadBuildInfo().AppVersion), () => CloseCleanlyAsync(skipUpdateWait: true));
            }
            catch (Exception error) { updates.ShowError(error); }
        }
        await App.GetService<IActivationService>().ActivateAsync(args);
        GetService<CoreHostService>().PropertyChanged += (_, change) =>
        {
            if (change.PropertyName == "ResourceActivation") updates.Refresh();
        };
        _ = updates.CheckAsync(automatic: true);
        // Players must not have to run a script: keep the Native Messaging host
        // registered for the current user on every start (best effort, per-user only).
        _ = Task.Run(() => { try { KuroBridgeRegistration.EnsureRegistered(); } catch { } });
        _ = GetService<KuroAutoSyncService>().InitializeAsync();
        if (ProgramLaunchSession.IsManaged) _ = ConfirmProgramHealthAsync(updates);
    }

    private async Task ConfirmProgramHealthAsync(UpdateUiController updates)
    {
        try
        {
            var snapshots = GetService<ResourceSnapshotService>();
            if (Environment.GetEnvironmentVariable("IMAO_LAUNCH_TRIAL") == "1")
            {
                using var timeout = new CancellationTokenSource(TimeSpan.FromMinutes(4));
                var core = GetService<CoreHostService>();
                await core.EnsureStartedAsync(timeout.Token);
                while (!core.IsConnected || !core.Status.ResourcesReady || core.Status.CoreState == "faulted" || core.Status.ResourceSnapshotId != snapshots.Current.SnapshotId)
                    await Task.Delay(100, timeout.Token);
                await snapshots.ReportHealthyAsync(snapshots.Current.SnapshotId, timeout.Token);
            }
            await ProgramLaunchSession.ReportHealthyAsync(ResourceUpdateBootstrap.ReadBuildInfo().AppVersion, snapshots.Current.SnapshotId);
            updates.Refresh();
        }
        catch (Exception error) { updates.ShowError(error); await CloseCleanlyAsync(); }
    }

    private async Task CloseCleanlyAsync(bool skipUpdateWait = false)
    {
        if (closing) return;
        closing = true;
        try
        {
            if (MainWindow.Content is UIElement content) content.IsHitTestVisible = false;
            if (!skipUpdateWait) await GetService<UpdateUiController>().CancelAndWaitAsync();
            await GetService<MapToolsController>().CloseAsync("软件正在退出", false);
            GetService<MapToolsController>().Dispose(); GetService<GamepadInputService>().Dispose();
            await GetService<CoreHostService>().DisposeAsync();
            if (GetService<ILocalSettingsService>() is LocalSettingsService settings) await settings.FlushAsync();
            closeApproved = true;
            // Keep the program lease until process exit, including any final native shutdown work.
            MainWindow.Close();
        }
        catch (Exception error)
        {
            closing = false;
            if (MainWindow.Content is UIElement content) content.IsHitTestVisible = true;
            GetService<UpdateUiController>().ShowError(error);
        }
    }

    /// <summary>
    /// Appends one line to the startup log. Startup work happens before the window is usable, so a slow step
    /// there is only diagnosable from a log; logging never throws into startup.
    /// </summary>
    private static void Trace(string line)
    {
        try
        {
            string directory = Path.Combine(UserDataPaths.Root, "Logs");
            Directory.CreateDirectory(directory);
            File.AppendAllText(Path.Combine(directory, "startup.log"), $"{DateTimeOffset.UtcNow:O} {line}{Environment.NewLine}");
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
    }
}
