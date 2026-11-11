using IMao_WinUI.Helpers;
using IMao_WinUI.Views;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace StartupPreparationRuntime;
public partial class App : Application
{
    public App() => InitializeComponent();
    protected override async void OnLaunched(LaunchActivatedEventArgs args)
    {
        using var log = new StreamWriter(Path.Combine(AppContext.BaseDirectory, "startup-ui-tests.log"), false) { AutoFlush = true };
        void Check(bool value, string name) { if (!value) throw new Exception(name); log.WriteLine("PASS " + name); }
        var view = new StartupPreparationView();
        var window = new Window { Title = "IMao startup verification", Content = view };
        view.AttachTitleBar(window);
        Check(window.ExtendsContentIntoTitleBar && view.TitleBar.Height == 34, "preparation uses the same custom title bar height as the shell");
        Check(window.AppWindow.TitleBar.ButtonBackgroundColor == Microsoft.UI.Colors.Transparent &&
            window.AppWindow.TitleBar.ButtonForegroundColor == Microsoft.UI.Colors.White,
            "preparation caption buttons use the dark shell colors before activation");
        window.Activate();
        var heartbeat = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(25) };
        int ticks = 0;
        heartbeat.Tick += (_, _) => ticks++;
        heartbeat.Start();
        try
        {
            bool firstFrame = false;
            int uiThread = Environment.CurrentManagedThreadId;
            int result = await StartupPreparation.RunAsync(async () =>
            {
                await view.WaitForFirstFrameAsync(CancellationToken.None);
                Check(view.IsLoaded && view.ActualWidth > 0 && view.ActualHeight > 0, "first window is laid out before resources start");
                var bitmap = new Microsoft.UI.Xaml.Media.Imaging.RenderTargetBitmap();
                await bitmap.RenderAsync(view);
                var pixels = await bitmap.GetPixelsAsync();
                var bytes = new byte[pixels.Length];
                using (var reader = Windows.Storage.Streams.DataReader.FromBuffer(pixels)) reader.ReadBytes(bytes);
                var folder = await Windows.Storage.StorageFolder.GetFolderFromPathAsync(AppContext.BaseDirectory);
                var file = await folder.CreateFileAsync("startup-titlebar.png", Windows.Storage.CreationCollisionOption.ReplaceExisting);
                using (var stream = await file.OpenAsync(Windows.Storage.FileAccessMode.ReadWrite))
                {
                    var encoder = await Windows.Graphics.Imaging.BitmapEncoder.CreateAsync(Windows.Graphics.Imaging.BitmapEncoder.PngEncoderId, stream);
                    encoder.SetPixelData(Windows.Graphics.Imaging.BitmapPixelFormat.Bgra8, Windows.Graphics.Imaging.BitmapAlphaMode.Premultiplied,
                        (uint)bitmap.PixelWidth, (uint)bitmap.PixelHeight, 96, 96, bytes);
                    await encoder.FlushAsync();
                }
                firstFrame = true;
            }, _ =>
            {
                if (!firstFrame || Environment.CurrentManagedThreadId == uiThread) throw new Exception("preparation began before frame or on UI thread");
                Thread.Sleep(450); // A deliberately synchronous disk-work stand-in.
                return Task.FromResult(7);
            }, CancellationToken.None);
            Check(result == 7 && ticks >= 3, "UI dispatcher remains responsive during synchronous preparation");
            try { await StartupPreparation.RunAsync<int>(() => Task.CompletedTask, _ => throw new IOException("fixture resource failure"), CancellationToken.None); }
            catch (IOException error) { view.ShowFailure(error); }
            await Task.Delay(100);
            Check(Descendants(view).OfType<TextBlock>().Any(t => t.Text.Contains("fixture resource failure")), "failure reason is shown in the window");
            Check(Descendants(view).OfType<ProgressRing>().All(p => !p.IsActive && p.Visibility == Visibility.Collapsed), "failed preparation does not keep spinning");
            using var close = new CancellationTokenSource();
            var childView = new StartupPreparationView();
            var child = new Window { Content = childView };
            child.Closed += (_, _) => close.Cancel();
            child.Activate();
            var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            var finish = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            bool published = false;
            var pending = StartupPreparation.RunAsync(() => childView.WaitForFirstFrameAsync(close.Token), async _ => { started.SetResult(); await finish.Task; return 1; }, close.Token);
            await started.Task;
            child.Close();
            finish.SetResult();
            try { await pending; published = true; } catch (OperationCanceledException) { }
            Check(close.IsCancellationRequested && !published, "closing during preparation prevents ready/shell publication");
            log.WriteLine("ALL STARTUP UI ASSERTIONS PASSED");
        }
        catch (Exception error) { log.WriteLine("FAIL " + error); Environment.ExitCode = 1; }
        finally { heartbeat.Stop(); window.Close(); }
    }

    private static IEnumerable<DependencyObject> Descendants(DependencyObject parent)
    {
        for (int i = 0; i < VisualTreeHelper.GetChildrenCount(parent); i++)
        {
            var child = VisualTreeHelper.GetChild(parent, i);
            yield return child;
            foreach (var descendant in Descendants(child)) yield return descendant;
        }
    }
}
