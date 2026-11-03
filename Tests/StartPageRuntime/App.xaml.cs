using IMao_WinUI.Helpers;
using IMao_WinUI.Services;
using IMao_WinUI.ViewModels;
using IMao_WinUI.Views;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using System.Reflection;

namespace StartPageRuntime;

public partial class App : Application
{
    public App() => InitializeComponent();

    protected override async void OnLaunched(LaunchActivatedEventArgs args)
    {
        string root = Path.Combine(AppContext.BaseDirectory, "fixture-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(Path.Combine(root, "catalogs"));
        Directory.CreateDirectory(Path.Combine(root, "states"));
        using var log = new StreamWriter(Path.Combine(AppContext.BaseDirectory, "start-page-tests.log")) { AutoFlush = true };
        var window = new Window();
        int checks = 0;
        void Check(bool ok, string message) { if (!ok) throw new Exception(message); ++checks; log.WriteLine("PASS " + message); }
        try
        {
            ResourceSessionPaths.MapDataRoot = root;
            File.WriteAllText(Path.Combine(root, "catalogs/catalog-8.json"), """[{"id":"3","name":"收集物","children":[{"id":"box","name":"测试宝箱"}]}]""");
            File.WriteAllText(Path.Combine(root, "states/state-8.json"), """[{"id":"box","location":[{"id":"a","stateId":8,"countryId":1,"x":0,"y":0},{"id":"b","stateId":8,"countryId":3,"x":0,"y":0}]}]""");
            var catalog = new LocalAccountCatalog(root);
            void WriteProgress(string id, string point) => File.WriteAllText(catalog.ProgressPath(id), "{\"points\":{\"8:" + point + "\":{\"completed\":true}}}");
            WriteProgress("a", "a");
            WriteProgress("b", "b");
            var core = new CoreHostService { LedgerCatalog = catalog };
            IMao_WinUI.App.Services[typeof(CoreHostService)] = core;
            IMao_WinUI.App.Services[typeof(StartViewModel)] = new StartViewModel();
            var page = new StartPage();
            var scroll = new ScrollViewer { Content = page };
            window.Content = scroll;
            window.AppWindow.Resize(new Windows.Graphics.SizeInt32(1050, 900));
            window.Activate();
            string Text(string name) => ((TextBlock)page.FindName(name)).Text;
            void Country(string id) => typeof(StartPage).GetMethod("CountryTab_Click", BindingFlags.NonPublic | BindingFlags.Instance)!
                .Invoke(page, [new Button { Tag = id }, new RoutedEventArgs()]);
            async Task Until(Func<bool> condition)
            {
                for (int i = 0; i < 100 && !condition(); ++i) await Task.Delay(50);
                Check(condition(), "expected page state arrived");
            }
            await Until(() => Text("SelectedRegionFractionText") == "已收集 1 / 1");
            Country("900");
            Check(Text("SelectedRegionTitleText") == "黑海岸", "empty country title replaces previous country");
            Check(((Panel)page.FindName("SubRegionTabsPanel")).Children.Count == 0, "empty country clears old region tabs");
            Check(!((ItemsControl)page.FindName("RegionCollectibleItemsControl")).Items.Any(), "empty country clears old collectible rows");
            Check(Text("SelectedRegionPercentText") == "—" && Text("SelectedRegionFractionText") == "暂无统计", "empty country clears old counts and percentage");
            Check(((ColumnDefinition)page.FindName("SelectedRegionFillCol")).Width.Value == 0, "empty country clears old progress fill");
            Country("1");
            Check(Text("SelectedRegionFractionText") == "已收集 1 / 1", "returning to populated country restores its data");

            WriteProgress("a", "b");
            core.CompletionChanged();
            await Until(() => Text("SelectedRegionFractionText") == "已收集 0 / 1");
            Country("3");
            Check(Text("SelectedRegionFractionText") == "已收集 1 / 1", "same-count completion change updates both countries");

            // A heartbeat must not synchronously parse progress or overwrite the last valid display.
            File.WriteAllText(catalog.ProgressPath("a"), "{");
            for (int i = 0; i < 20; ++i) core.Heartbeat();
            Check(Text("SelectedRegionFractionText") == "已收集 1 / 1", "heartbeats do not synchronously read progress");
            core.CompletionChanged();
            await Until(() => Text("RegionCollectibleEmptyText").Contains("无法读取"));
            Check(Text("SelectedRegionPercentText") == "—", "read failure is not misrepresented as zero percent");
            WriteProgress("a", "a");
            core.CompletionChanged();
            await Until(() => Text("SelectedRegionFractionText") == "已收集 0 / 1");
            core.SelectLedger("b");
            Check(Text("SelectedRegionFractionText") == "暂无统计", "account switch clears previous account before asynchronous read");
            await Until(() => Text("SelectedRegionFractionText") == "已收集 1 / 1");
            core.SelectLedger("a");
            core.SelectLedger("b");
            await Until(() => Text("SelectedRegionFractionText") == "已收集 1 / 1");
            await Task.Delay(1200);
            Check(Text("SelectedRegionFractionText") == "已收集 1 / 1", "rapid account switches cannot apply an obsolete result");
            window.Content = null;
            await Task.Delay(80);
            var timer = (DispatcherTimer)typeof(StartPage).GetField("progressTimer", BindingFlags.NonPublic | BindingFlags.Instance)!.GetValue(page)!;
            Check(!timer.IsEnabled, "leaving home stops progress polling");
            window.Content = scroll;
            await Until(() => Text("SelectedRegionFractionText") == "已收集 1 / 1" && timer.IsEnabled);
            log.WriteLine($"ALL {checks} START PAGE CHECKS PASSED");
        }
        catch (Exception ex) { log.WriteLine("FAIL " + ex); Environment.ExitCode = 1; }
        finally { window.Close(); Exit(); }
    }
}
