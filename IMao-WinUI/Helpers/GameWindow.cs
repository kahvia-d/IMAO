using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Drawing;

// 本地客户端游戏窗口状态枚举，用于区分“未启动”、“已最小化”与“窗口过小”
public enum GameWindowCheckResult
{
    Ready,
    ProcessNotFound,
    WindowMinimizedOrHidden,
    WindowTooSmall
}

class WindowClientSizeGetter
{
    // 引入 Windows API
    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsProc enumProc, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);

    [DllImport("user32.dll")]
    private static extern bool GetClientRect(IntPtr hWnd, out RECT lpRect);

    private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool IsIconic(IntPtr hWnd);

    // 客户区矩形结构
    private struct RECT
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    /// <summary>
    /// 检测目标进程窗口的详细状态及最大可见客户区尺寸
    /// </summary>
    public static GameWindowCheckResult InspectProcessWindow(string processName, out Size largestClientSize)
    {
        largestClientSize = Size.Empty;
        try
        {
            var processIds = Process.GetProcessesByName(processName)
                .Select(process => (uint)process.Id)
                .ToHashSet();
            if (processIds.Count == 0)
            {
                return GameWindowCheckResult.ProcessNotFound;
            }

            long largestValidArea = 0;
            long largestVisibleArea = 0;
            Size largestVisibleSize = Size.Empty;
            bool hasMinimizedWindow = false;
            Size localLargestValid = Size.Empty;

            EnumWindows((hWnd, lParam) =>
            {
                GetWindowThreadProcessId(hWnd, out uint processId);
                if (!processIds.Contains(processId))
                {
                    return true;
                }

                if (IsIconic(hWnd))
                {
                    hasMinimizedWindow = true;
                    return true;
                }

                if (IsWindowVisible(hWnd) && GetClientRect(hWnd, out RECT clientRect))
                {
                    int width = clientRect.Right - clientRect.Left;
                    int height = clientRect.Bottom - clientRect.Top;
                    long area = (long)width * height;
                    if (width > 0 && height > 0 && area > largestVisibleArea)
                    {
                        largestVisibleArea = area;
                        largestVisibleSize = new Size(width, height);
                    }
                    if (width >= 640 && height >= 360 && area > largestValidArea)
                    {
                        localLargestValid = new Size(width, height);
                        largestValidArea = area;
                    }
                }
                return true;
            }, IntPtr.Zero);

            if (localLargestValid != Size.Empty)
            {
                largestClientSize = localLargestValid;
                return GameWindowCheckResult.Ready;
            }

            if (largestVisibleSize != Size.Empty)
            {
                largestClientSize = largestVisibleSize;
                return GameWindowCheckResult.WindowTooSmall;
            }

            return hasMinimizedWindow
                ? GameWindowCheckResult.WindowMinimizedOrHidden
                : GameWindowCheckResult.ProcessNotFound;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"InspectProcessWindow: {ex.Message}");
            return GameWindowCheckResult.ProcessNotFound;
        }
    }

    /// <summary>
    /// 通过进程名获取窗口客户区大小（不含非客户区域）
    /// </summary>
    /// <param name="processName">进程名（不含.exe）</param>
    /// <returns>客户区大小，未找到返回Size.Empty</returns>
    public static Size GetClientSizeByProcessName(string processName)
    {
        return InspectProcessWindow(processName, out Size size) == GameWindowCheckResult.Ready
            ? size
            : Size.Empty;
    }
}

class GameWindow()
{
    /// <summary>
    /// 游戏窗口现在能不能开始探索：目标进程在跑，并且有一个可见、未最小化、客户区不小于 640x360 的游戏窗口。
    /// </summary>
    public static bool CheckGameWindowSize()
        => WindowClientSizeGetter.GetClientSizeByProcessName("Client-Win64-Shipping") != Size.Empty;

    /// <summary>
    /// 返回本地游戏客户端窗口的精确检测结果，用于向玩家给出准确的错误提示
    /// </summary>
    public static GameWindowCheckResult InspectGameWindow(out Size clientSize)
        => WindowClientSizeGetter.InspectProcessWindow("Client-Win64-Shipping", out clientSize);

    /// <summary>
    /// 根据窗口检测结果生成对应场景的提示标题与说明
    /// </summary>
    public static (string Title, string Message) DescribeFailure(GameWindowCheckResult checkResult) => checkResult switch
    {
        GameWindowCheckResult.ProcessNotFound => (
            "未检测到游戏窗口",
            "请确认《鸣潮》已启动；若正在使用浏览器游玩云鸣潮，请先点击「选择游戏窗口」。"),
        GameWindowCheckResult.WindowMinimizedOrHidden => (
            "游戏窗口未显示",
            "检测到《鸣潮》正在运行，但窗口处于最小化或隐藏状态，请先恢复游戏窗口后再开始探索。"),
        GameWindowCheckResult.WindowTooSmall => (
            "游戏窗口过小",
            "当前游戏画面尺寸过小，为保证地图识别精度，请将游戏窗口分辨率调整到至少 1280×720。"),
        _ => ("无法开始探索", "请确认游戏窗口已正常显示。")
    };
}
