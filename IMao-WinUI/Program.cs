using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using IMao_WinUI.Core.Updates;
using IMao_WinUI.Helpers;

namespace IMao_WinUI;

public static class Program
{
    [STAThread]
    public static void Main(string[] args)
    {
        StartupLog.Write("entry-start");
        try
        {
            if (ProgramEntryRedirect.TryRedirect(AppContext.BaseDirectory, ProgramLaunchSession.IsManaged,
                File.Exists, launcher =>
                {
                    using var process = Process.Start(new ProcessStartInfo(launcher)
                    {
                        UseShellExecute = true, WorkingDirectory = Path.GetDirectoryName(launcher)!
                    });
                    if (process is null) throw new IOException("无法启动程序更新器。");
                }))
            {
                StartupLog.Write("entry-launcher-handoff");
                return;
            }
        }
        catch (Exception error)
        {
            StartupLog.Write("entry-launcher-failed " + error);
            MessageBox(IntPtr.Zero, "无法启动程序更新器：" + error.Message, "IMao 启动失败", 0x10);
            return;
        }
        StartApplication();
    }

    // Keep the handoff path free from XAML initialization and JIT loading of its entry method.
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void StartApplication()
    {
        WinRT.ComWrappersSupport.InitializeComWrappers();
        Microsoft.UI.Xaml.Application.Start(parameters =>
        {
            SynchronizationContext.SetSynchronizationContext(
                new Microsoft.UI.Dispatching.DispatcherQueueSynchronizationContext(
                    Microsoft.UI.Dispatching.DispatcherQueue.GetForCurrentThread()));
            _ = new App();
        });
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int MessageBox(IntPtr owner, string text, string caption, uint type);
}
