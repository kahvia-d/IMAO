using Microsoft.UI.Xaml;

namespace GuideWindowRuntime;

public partial class App : Application
{
    private Window? lifetimeWindow;
    public App() => InitializeComponent();

    protected override async void OnLaunched(LaunchActivatedEventArgs args)
    {
        bool readerProbe = Environment.GetCommandLineArgs().Contains("--probe-gamepad-reader");
        bool serviceTest = Environment.GetCommandLineArgs().Contains("--test-gamepad-service");
        bool crossProcessTest = Environment.GetCommandLineArgs().Contains("--test-gamepad-cross-process");
        bool visibilityTest = Environment.GetCommandLineArgs().Contains("--test-assistant-visibility");
        bool routeControllerTest = Environment.GetCommandLineArgs().Contains("--test-route-controller");
        bool nearbyTest = Environment.GetCommandLineArgs().Contains("--test-nearby-chooser");
        bool cursorTest = Environment.GetCommandLineArgs().Contains("--test-cursor-candidates");
        bool returnTest = Environment.GetCommandLineArgs().Contains("--test-gamepad-return");
        bool refusalProbe = Environment.GetCommandLineArgs().Contains("--probe-gamepad-return-refusal");
        int sourceArgument = Array.IndexOf(Environment.GetCommandLineArgs(), "--foreground-source");
        if (sourceArgument >= 0)
        {
            try { await GamepadForegroundSource.RunAsync(Environment.GetCommandLineArgs()[sourceArgument + 1],
                Environment.GetCommandLineArgs().Contains("--lock-foreground"),
                Environment.GetCommandLineArgs().Contains("--topmost-source")); }
            catch (Exception error) { Environment.ExitCode = 1; System.Diagnostics.Debug.WriteLine(error); }
            Exit();
            return;
        }
        nint originalForeground = readerProbe ? GamepadReaderProbe.CurrentForeground : 0;
        // Keep the dispatcher alive while each tested guide is disposed. This window is never activated.
        lifetimeWindow = new Window { Title = "Guide integration test lifetime" };
        bool preview = Environment.GetCommandLineArgs().Contains("--preview");
        string logPath = Path.Combine(AppContext.BaseDirectory, readerProbe ? "gamepad-reader-probe.jsonl" :
            serviceTest ? "gamepad-service-tests.log" :
            crossProcessTest ? "gamepad-cross-process-tests.log" :
            visibilityTest ? "assistant-visibility-tests.log" :
            routeControllerTest ? "route-controller-tests.log" :
            nearbyTest ? "nearby-chooser-tests.log" :
            cursorTest ? "cursor-candidate-tests.log" :
            refusalProbe ? "gamepad-return-refusal-probe.log" :
            returnTest ? "gamepad-return-tests.log" :
            preview ? "guide-window-preview.log" : "guide-window-tests.log");
        using var log = new StreamWriter(logPath, append: false) { AutoFlush = true };
        int exitCode = 1;
        try
        {
            if (readerProbe) await GamepadReaderProbe.RunAsync(originalForeground, message => log.WriteLine(message));
            else if (serviceTest) await GamepadWindowTests.RunServiceAsync(message => log.WriteLine(message));
            else if (crossProcessTest) await GamepadCrossProcessTests.RunAsync(message => log.WriteLine(message));
            else if (visibilityTest) await AssistantVisibilityTests.RunAsync(message => log.WriteLine(message));
            else if (routeControllerTest) await RouteControllerTests.RunAsync(message => log.WriteLine(message));
            else if (nearbyTest) await NearbyChooserTests.RunAsync(message => log.WriteLine(message));
            else if (cursorTest) await CursorCandidateTests.RunAsync(message => log.WriteLine(message));
            else if (refusalProbe) await GamepadReturnTests.ProbeRefusalAsync(message => log.WriteLine(message));
            else if (returnTest) await GamepadReturnTests.RunAsync(message => log.WriteLine(message));
            else if (preview) await GuideWindowTests.PreviewAsync(message => log.WriteLine(message));
            else
            {
                await GuideWindowTests.RunAsync(message => log.WriteLine(message));
                await GamepadWindowTests.RunAsync(message => log.WriteLine(message));
            }
            log.WriteLine(readerProbe ? "Gamepad reader probe completed." : serviceTest ? "Real gamepad service integration tests passed." :
                refusalProbe ? "Return refusal probe completed." :
                crossProcessTest ? "Cross-process gamepad activation tests passed." :
                visibilityTest ? "Assistant visibility tests passed." :
                routeControllerTest ? "Route controller window tests passed." :
                returnTest ? "Cross-process return and handoff tests passed." :
                cursorTest ? "Cursor candidate window tests passed." :
                preview ? "Controlled guide preview completed." : "All real WinUI guide window tests passed.");
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
