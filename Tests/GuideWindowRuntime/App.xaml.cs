using Microsoft.UI.Xaml;

namespace GuideWindowRuntime;

public partial class App : Application
{
    private Window? lifetimeWindow;
    public App() => InitializeComponent();

    protected override async void OnLaunched(LaunchActivatedEventArgs args)
    {
        // Keep the dispatcher alive while each tested guide is disposed. This window is never activated.
        lifetimeWindow = new Window { Title = "Guide integration test lifetime" };
        bool preview = Environment.GetCommandLineArgs().Contains("--preview");
        string logPath = Path.Combine(AppContext.BaseDirectory, preview ? "guide-window-preview.log" : "guide-window-tests.log");
        using var log = new StreamWriter(logPath, append: false) { AutoFlush = true };
        int exitCode = 1;
        try
        {
            if (preview) await GuideWindowTests.PreviewAsync(message => log.WriteLine(message));
            else await GuideWindowTests.RunAsync(message => log.WriteLine(message));
            log.WriteLine(preview ? "Controlled guide preview completed." : "All real WinUI guide window tests passed.");
            exitCode = 0;
        }
        catch (Exception error) { log.WriteLine("FAIL " + error); }
        finally
        {
            Environment.ExitCode = exitCode;
            log.Flush();
            lifetimeWindow.Close();
            Exit();
        }
    }
}
