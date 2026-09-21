using IMao_WinUI.Models;
using Microsoft.UI.Dispatching;
using System.Runtime.InteropServices;

namespace IMao_WinUI.Services;

/// <summary>
/// 键盘侧的「开始探索 / 停止探索」快捷键（默认 F9，可在设置里改；手柄那一半是 LB+按下RS，
/// 由 GamepadInputService 走同一份 CoreHostService.ToggleExplorationAsync）。
///
/// 为什么轮询放在**外壳**而不是核心：快捷键要能"开始探索"，而核心停止时它内部的轮询线程也一起停了，
/// 那样关掉之后就再也没人能把工具打开。外壳始终在运行，所以它是唯一能同时负责开与关的地方。
/// 用 GetAsyncKeyState 的"当前是否按下"位自己做边沿检测：低位的"自上次调用后按过"是按线程算的，
/// 在被别的线程反复调用的进程里不可靠。
/// </summary>
public sealed class ExplorationHotkeyService : IDisposable
{
    private readonly CoreHostService coreHost;
    private readonly DispatcherQueueTimer timer;
    private bool lastPressed;
    private bool toggling;
    private bool disposed;

    public ExplorationHotkeyService(CoreHostService coreHost)
    {
        this.coreHost = coreHost;
        timer = DispatcherQueue.GetForCurrentThread().CreateTimer();
        timer.Interval = TimeSpan.FromMilliseconds(50);
        timer.Tick += OnTick;
        timer.Start();
        coreHost.PropertyChanged += OnCoreHostPropertyChanged;
    }

    private void OnCoreHostPropertyChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        // 配置变化（例如玩家改了快捷键）不需要重启计时器：每次 tick 都重新读当前绑定。
        if (e.PropertyName == nameof(CoreHostService.Configuration)) lastPressed = false;
    }

    private async void OnTick(DispatcherQueueTimer sender, object args)
    {
        if (disposed) return;
        try
        {
            var key = coreHost.Configuration.ToggleEnabledKey;
            // 0 表示玩家在设置里把它禁用了。
            var pressed = key > 0 && (GetAsyncKeyState(key) & 0x8000) != 0;
            var fresh = pressed && !lastPressed;
            lastPressed = pressed;
            if (!fresh || toggling) return;
            toggling = true;
            try
            {
                var result = await coreHost.ToggleExplorationAsync(GameWindow.CheckGameWindowSize());
                if (result == ExplorationToggleResult.WindowSizeRejected)
                    coreHost.ReportUserError("游戏窗口尺寸不合适，无法开始探索（与首页按钮同样的要求）。");
            }
            finally { toggling = false; }
        }
        catch (Exception exception) when (exception is IOException or InvalidOperationException or ArgumentException or OperationCanceledException)
        { coreHost.ReportUserError("快捷键切换探索失败：" + exception.Message); }
    }

    [DllImport("user32.dll")]
    private static extern short GetAsyncKeyState(int vKey);

    public void Dispose()
    {
        if (disposed) return;
        disposed = true;
        timer.Stop();
        timer.Tick -= OnTick;
        coreHost.PropertyChanged -= OnCoreHostPropertyChanged;
    }
}
