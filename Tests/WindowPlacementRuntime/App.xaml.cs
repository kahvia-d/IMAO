using IMao_WinUI;
using IMao_WinUI.Helpers;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Windows.Graphics;
using System.Runtime.InteropServices;
namespace WindowPlacementRuntime;
public partial class App : Application
{
 private Window? keeper;
 public App() { InitializeComponent(); }
 protected override async void OnLaunched(LaunchActivatedEventArgs args)
 {
  string fixture = Path.Combine(Path.GetTempPath(),"IMao-placement-runtime-"+Guid.NewGuid().ToString("N"));
  Directory.CreateDirectory(fixture);
  UserDataPaths.Root = Path.Combine(fixture,"user");
  using var log = new StreamWriter(Path.Combine(Path.GetTempPath(),"IMao-window-placement-runtime.log"),false) { AutoFlush=true };
  int checks=0;
  void Check(bool ok,string name) { if(!ok) throw new Exception(name); checks++; log.WriteLine("PASS "+name); }
  string[] Inventory() => Directory.GetFiles(AppContext.BaseDirectory,"*",SearchOption.AllDirectories).Order().ToArray();
  var inventory=Inventory();
  try {
   keeper=new Window(); keeper.Activate(); keeper.AppWindow.Hide();
   var first=new MainWindow();
   var work=DisplayArea.GetFromWindowId(first.AppWindow.Id,DisplayAreaFallback.Nearest).WorkArea;
   var size=first.AppWindow.Size; var pos=first.AppWindow.Position;
   Check(Math.Abs(pos.X-(work.X+(work.Width-size.Width)/2))<=1 && Math.Abs(pos.Y-(work.Y+(work.Height-size.Height)/2))<=1,"first constructor centers before activation");
   first.Activate(); await Task.Delay(180);
   uint dpi=GetDpiForWindow(WinRT.Interop.WindowNative.GetWindowHandle(first));
   int width=Math.Min(work.Width,(int)(850*dpi/96d)); int height=Math.Min(work.Height,(int)(550*dpi/96d));
   var desired=new RectInt32(work.X+Math.Min(47,work.Width-width),work.Y+Math.Min(39,work.Height-height),width,height);
   first.AppWindow.MoveAndResize(desired); await Task.Delay(180);
   var expected=new PlacementBounds(first.AppWindow.Position.X,first.AppWindow.Position.Y,first.AppWindow.Size.Width,first.AppWindow.Size.Height);
   first.Close(); await Task.Delay(100);
   var errors=new List<string>(); var store=new MainWindowPlacementStore(UserDataPaths.Root,errors.Add);
   Check(store.Load()?.Bounds==expected,"normal close commits actual window geometry");
   var second=new MainWindow();
   Check(Bounds(second)==expected,"new window restores persisted geometry before activation");
   second.Activate(); await Task.Delay(120);
   ((OverlappedPresenter)second.AppWindow.Presenter).Maximize(); await Task.Delay(180);
   second.Close(); await Task.Delay(100);
   Check(store.Load()?.Bounds==expected,"closing maximized retains last normal geometry");
   var third=new MainWindow(); third.Activate(); await Task.Delay(120);
   Check(Bounds(third)==expected && ((OverlappedPresenter)third.AppWindow.Presenter).State==OverlappedPresenterState.Restored,"maximized close restarts at normal bounds");
   ((OverlappedPresenter)third.AppWindow.Presenter).Minimize(); await Task.Delay(180);
   third.Close(); await Task.Delay(100);
   Check(store.Load()?.Bounds==expected,"closing minimized retains last normal geometry");
   var fourth=new MainWindow(); fourth.Activate(); await Task.Delay(120);
   Check(Bounds(fourth)==expected && ((OverlappedPresenter)fourth.AppWindow.Presenter).State==OverlappedPresenterState.Restored,"minimized close never restarts minimized");
   var changed = new RectInt32((int)expected.X+5,(int)expected.Y+5,(int)expected.Width,(int)expected.Height);
   fourth.AppWindow.MoveAndResize(changed); await Task.Delay(100);
   void CancelClose(AppWindow sender, AppWindowClosingEventArgs e) => e.Cancel=true;
   fourth.AppWindow.Closing += CancelClose;
   SendMessage(WinRT.Interop.WindowNative.GetWindowHandle(fourth),0x0010,IntPtr.Zero,IntPtr.Zero); await Task.Delay(100);
   Check(store.Load()?.Bounds==expected,"canceled close leaves previous durable placement unchanged");
   fourth.AppWindow.Closing -= CancelClose;
   fourth.Close(); await Task.Delay(100);
   Check(inventory.SequenceEqual(Inventory()),"window lifecycle adds no executable-directory files");
   Check(errors.Count==0 && Directory.GetFiles(UserDataPaths.Root,"*.json").Length==1 && !Directory.GetFiles(UserDataPaths.Root,"*.tmp-*").Any(),"only atomic placement JSON under fixture user root");
   log.WriteLine($"ALL {checks} WINDOW LIFECYCLE ASSERTIONS PASSED");
  } catch(Exception error) {log.WriteLine("FAIL "+error); Environment.ExitCode=1;}
  finally { keeper?.Close(); Directory.Delete(fixture,true); Exit(); }
 }
 static PlacementBounds Bounds(MainWindow w)=>new(w.AppWindow.Position.X,w.AppWindow.Position.Y,w.AppWindow.Size.Width,w.AppWindow.Size.Height);
 [DllImport("user32.dll")] static extern uint GetDpiForWindow(IntPtr window);
 [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr window,uint message,IntPtr wParam,IntPtr lParam);
}
