using System.Collections.Specialized;
using System.Web;

using IMao_WinUI.Contracts.Services;
using IMao_WinUI.Helpers;
using IMao_WinUI.ViewModels;

using Microsoft.Windows.AppNotifications;

namespace IMao_WinUI.Notifications;

public class AppNotificationService : IAppNotificationService
{
    private readonly INavigationService _navigationService;

    public AppNotificationService(INavigationService navigationService)
    {
        _navigationService = navigationService;
    }

    // Called from the finalizer below, where an escaping exception terminates the process. Keep it
    // fail-soft: a player would only see the program vanish while closing it.
    ~AppNotificationService()
    {
        Unregister();
    }

    /// <summary>
    /// Registers for notifications. Every failure here is expected somewhere and is never fatal.
    /// </summary>
    /// <remarks>
    /// App notifications are the most fragile step in startup, and nothing the program does today needs
    /// them (no caller of <see cref="Show"/>): an unpackaged self-contained app needs the Windows App
    /// Runtime Main/Singleton packages for it - without them <see cref="AppNotificationManager.Default"/>
    /// throws E_ACCESSDENIED - and Microsoft documents app notifications as unsupported for elevated
    /// (admin) processes, which this program's manifest requires. On 2026-10-12 a player whose machine
    /// had never had those packages lost every released version to this call: it ran in the App
    /// constructor, so the exception became a stowed exception (0xC000027B) and killed the process
    /// before any window, log or error message could exist.
    /// </remarks>
    public void Initialize()
    {
        try
        {
            if (!AppNotificationManager.IsSupported())
            {
                StartupLog.Write("notification-unsupported");
                return;
            }
            AppNotificationManager.Default.NotificationInvoked += OnNotificationInvoked;

            AppNotificationManager.Default.Register();
        }
        catch (Exception error)
        {
            // A missing runtime package, an elevated process, a managed or sand-boxed environment: all of
            // them mean "no notifications", none of them mean "cannot start".
            StartupLog.Write("notification-init-failed " + error);
        }
    }

    public void OnNotificationInvoked(AppNotificationManager sender, AppNotificationActivatedEventArgs args)
    {
        // TODO: Handle notification invocations when your app is already running.

        //// // Navigate to a specific page based on the notification arguments.
        //// if (ParseArguments(args.Argument)["action"] == "Settings")
        //// {
        ////    App.MainWindow.DispatcherQueue.TryEnqueue(() =>
        ////    {
        ////        _navigationService.NavigateTo(typeof(SettingsViewModel).FullName!);
        ////    });
        //// }

        App.MainWindow.DispatcherQueue.TryEnqueue(() =>
        {
            App.MainWindow.ShowMessageDialogAsync("TODO: Handle notification invocations when your app is already running.", "Notification Invoked");

            App.MainWindow.BringToFront();
        });
    }

    public bool Show(string payload)
    {
        try
        {
            var appNotification = new AppNotification(payload);

            AppNotificationManager.Default.Show(appNotification);

            return appNotification.Id != 0;
        }
        catch (Exception error)
        {
            StartupLog.Write("notification-show-failed " + error);
            return false;
        }
    }

    public NameValueCollection ParseArguments(string arguments)
    {
        return HttpUtility.ParseQueryString(arguments);
    }

    public void Unregister()
    {
        try
        {
            AppNotificationManager.Default.Unregister();
        }
        catch (Exception error)
        {
            StartupLog.Write("notification-unregister-failed " + error);
        }
    }
}
