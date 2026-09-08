using IMao_WinUI.Contracts.Services;
using IMao_WinUI.Services;
using IMao_WinUI.ViewModels;
using IMao_WinUI.Views;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using System.Reflection;
using Windows.Graphics;
using Windows.Graphics.Imaging;
using Windows.Storage;
using System.Runtime.InteropServices.WindowsRuntime;
using TestApp=IMao_WinUI.App;

namespace MainWindowRuntime;
public partial class App : Application
{
    public App() { InitializeComponent(); UnhandledException += (_,e)=> { File.AppendAllText(Path.Combine(AppContext.BaseDirectory,"ui-tests.log"),"UNHANDLED "+e.Exception+"\n"); }; }
    protected override async void OnLaunched(LaunchActivatedEventArgs args)
    {
        using var log = new StreamWriter(Path.Combine(AppContext.BaseDirectory,"ui-tests.log"),false) { AutoFlush=true };
        var window=new Window { Title="IMao · 独立界面验收" };
        TestApp.MainWindow=window;
        var core=new CoreHostService();
        var pageService=new PageService();
        var navigation=new NavigationService(pageService);
        var navView=new NavigationViewService(navigation,pageService);
        TestApp.Services[typeof(CoreHostService)]=core;
        TestApp.Services[typeof(FilterSelectionService)]=new FilterSelectionService(core);
        TestApp.Services[typeof(GamepadInputService)]=new GamepadInputService();
        TestApp.Services[typeof(INavigationService)]=navigation;
        foreach(var type in new[] { typeof(StartViewModel),typeof(FilterViewModel),typeof(FunctionViewModel),typeof(SettingsViewModel),typeof(UsageGuideViewModel),typeof(DiagnosticsViewModel) }) TestApp.Services[type]=Activator.CreateInstance(type)!;
        var shell=new ShellPage(new ShellViewModel(navigation,navView)) { RequestedTheme=ElementTheme.Dark };
        window.Content=shell;window.AppWindow.Resize(new SizeInt32(1120,780));window.Activate();
        int assertions=0;
        void Check(bool valid,string message) { if(!valid) throw new Exception(message); assertions++; log.WriteLine("PASS "+message); }
        try
        {
            if (Environment.GetCommandLineArgs().Contains("--test-filter-sharing"))
            {
                await FilterSharingTests.RunAsync(window, Check);
                log.WriteLine("ALL " + assertions + " FILTER ASSERTIONS PASSED");
                Environment.ExitCode = 0;
                return;
            }
            var normal=new RectInt32(120,90,900,650);
            var kept=IMao_WinUI.Helpers.MainWindowPlacement.Fit(normal,new(0,0,1920,1040));
            Check(kept.X==120&&kept.Y==90&&kept.Width==900&&kept.Height==650,"restored dimensions and position remain unchanged when visible");
            var fitted=IMao_WinUI.Helpers.MainWindowPlacement.Fit(new(-2400,900,2240,1560),new(-1920,0,1920,1040));
            Check(fitted.X==-1920&&fitted.Y==0&&fitted.Width==1920&&fitted.Height==1040,"200 percent large window fits negative-coordinate monitor work area");
            await Task.Delay(300);
            var nav=(NavigationView)shell.FindName("NavigationViewControl");
            Check(nav.MenuItems.Count==3&&nav.FooterMenuItems.Count==3,"three primary and three secondary destinations");
            var kinds=new[]{typeof(StartViewModel),typeof(FilterViewModel),typeof(FunctionViewModel),typeof(SettingsViewModel),typeof(UsageGuideViewModel),typeof(DiagnosticsViewModel)};
            foreach(var size in new[]{new SizeInt32(1120,780),new SizeInt32(800,500)})
            {
                var scale=shell.XamlRoot.RasterizationScale;
                window.AppWindow.Resize(new SizeInt32((int)(size.Width*scale),(int)(size.Height*scale)));
                await Task.Delay(220);
                foreach(var kind in kinds)
                {
                    navigation.NavigateTo(kind.FullName!);
                    await Task.Delay(650);
                    shell.UpdateLayout();
                    var page=(Page)navigation.Frame!.Content;
                    Check(page.IsLoaded&&page.ActualWidth>450&&page.ActualHeight>100,$"{size.Width}x{size.Height} {page.GetType().Name} is rendered");
                    Check(ReferenceEquals(shell.ViewModel.Selected,navView.GetSelectedItem(page.GetType())),$"{page.GetType().Name} navigation selection matches");
                    var outer=(ScrollViewer)shell.FindName("PageScrollViewer");
                    Check(outer.ScrollableWidth<1,$"{page.GetType().Name} no horizontal page scrolling");
                    if(page is FilterPage filter)
                    {
                        var grid=(GridView)filter.Filter.FindName("FilterItems");
                        Check(grid.Items.Count==600,"all 600 filter rows retained");
                        Check(grid.ActualHeight>70&&double.IsFinite(grid.ActualHeight),"filter viewport remains bounded");
                        int realized=Enumerable.Range(0,600).Count(i=>grid.ContainerFromIndex(i)!=null);
                        Check(realized<200,"filter virtualization retained ("+realized+"/600)");
                        var box=(TextBox)filter.Filter.FindName("SearchBox");box.Text="长的";
                        await Task.Delay(80);
                        Check(grid.Items.Count==150,"search still filters actual catalog rows");
                        box.Text=""; await Task.Delay(100);
                    }
                    if(page is DiagnosticsPage)
                    {
                        var list=(ListView)page.FindName("Diagnostics_LogList");
                        Check(list.ActualHeight>20,"diagnostic log viewport stays visible");
                    }
                    await Capture(shell,$"{size.Width}-{page.GetType().Name}.png");
                    if(size.Width==800)
                    {
                        string? controlName=page switch { FilterPage=>"SelectResults",FunctionPage=>"AutoRouteGuide",SettingsPage=>"SaveShortcuts",_=>null };
                        if(controlName is not null)
                        {
                            var control=(FrameworkElement)(page is FilterPage filterPage ? filterPage.Filter.FindName(controlName) : page.FindName(controlName));
                            control.StartBringIntoView(new BringIntoViewOptions {AnimationDesired=false});
                            await Task.Delay(200);shell.UpdateLayout();
                            var bounds=control.TransformToVisual(shell).TransformBounds(new Windows.Foundation.Rect(0,0,control.ActualWidth,control.ActualHeight));
                            Check(bounds.Left>=0&&bounds.Right<=shell.ActualWidth+1&&bounds.Top>=0&&bounds.Bottom<=shell.ActualHeight+1,controlName+" is fully reachable at minimum window size");
                            await Capture(shell,$"800-{page.GetType().Name}-action.png");
                        }
                    }
                    log.WriteLine($"METRIC {page.GetType().Name} actual={page.ActualWidth:F0}x{page.ActualHeight:F0}; viewport={outer.ViewportWidth:F0}x{outer.ViewportHeight:F0}; scroll={outer.ScrollableHeight:F0}");
                }
            }
            navigation.NavigateTo(typeof(SettingsViewModel).FullName!);await Task.Delay(120);
            var settings=(SettingsPage)navigation.Frame!.Content;
            var enabled=(ToggleSwitch)settings.FindName("Setting_MapShowItem");
            enabled.IsOn=false;await Task.Delay(80);Check(!core.Configuration.MapEnabled,"moved display toggle updates configuration");
            var automatic=(ToggleSwitch)settings.FindName("AutomaticReplan");
            automatic.IsOn=true;await Task.Delay(80);Check(core.Configuration.AutoReplanEnabled,"settings auto replan toggle updates configuration");
            navigation.NavigateTo(typeof(FunctionViewModel).FullName!);await Task.Delay(120);
            var routes=(FunctionPage)navigation.Frame!.Content;
            var routeToggle=(ToggleSwitch)routes.FindName("AutoReplanToggle");
            Check(routeToggle.IsOn,"route toggle restores shared setting");
            await core.ConfigureAsync(autoReplanEnabled:false);await Task.Delay(50);
            Check(!routeToggle.IsOn,"route toggle follows external configuration updates");
            core.RejectConfigure=true;routeToggle.IsOn=true;await Task.Delay(80);
            Check(!routeToggle.IsOn&&((InfoBar)routes.FindName("AutoRouteMessage")).IsOpen,"route failed setting restores value and reports failure");core.RejectConfigure=false;
            var stop=new IMao_WinUI.Models.RouteStop { Key="3:point-17",PointId="point-17",Name="测试目标" };
            core.PublishRoute(new() {ProfileId="fixture",Active=new(){Id="route-9",Name="测试路线",Stops=[stop]},CurrentTarget=stop});
            typeof(FunctionPage).GetMethod("AutoRouteAction_Click",BindingFlags.NonPublic|BindingFlags.Instance)!.Invoke(routes,[new Button {Tag="complete"},new RoutedEventArgs()]);
            await Task.Delay(50);var command=core.RouteCommands.Last();
            var json=System.Text.Json.JsonSerializer.Serialize(command.Arguments);
            Check(command.Action=="complete"&&json.Contains("point-17")&&json.Contains("route-9")&&json.Contains("fixture"),"route completion keeps exact target/profile/route identity");
            navigation.NavigateTo(typeof(StartViewModel).FullName!);await Task.Delay(80);
            var home=(StartPage)navigation.Frame!.Content;
            Check(((TextBlock)home.FindName("OverviewRouteTitle")).Text=="测试路线","overview shows active route");
            Check(((TextBlock)home.FindName("OverviewRouteTarget")).Text.Contains("测试目标"),"overview shows actual current target");
            await FilterSharingTests.RunAsync(window, Check);
            log.WriteLine("ALL "+assertions+" ASSERTIONS PASSED");
            Environment.ExitCode=0;
        }
        catch(Exception error) { log.WriteLine("FAIL "+error);Environment.ExitCode=1; }
        finally { window.Close();Exit(); }
    }
    internal static async Task Capture(FrameworkElement root,string name)
    {
        var bitmap=new RenderTargetBitmap();await bitmap.RenderAsync(root);
        var buffer=await bitmap.GetPixelsAsync();
        var folder=await StorageFolder.GetFolderFromPathAsync(AppContext.BaseDirectory);
        var file=await folder.CreateFileAsync(name,CreationCollisionOption.ReplaceExisting);
        using var stream=await file.OpenAsync(FileAccessMode.ReadWrite);
        var encoder=await BitmapEncoder.CreateAsync(BitmapEncoder.PngEncoderId,stream);
        encoder.SetPixelData(BitmapPixelFormat.Bgra8,BitmapAlphaMode.Premultiplied,(uint)bitmap.PixelWidth,(uint)bitmap.PixelHeight,96,96,buffer.ToArray());
        await encoder.FlushAsync();
    }
}
