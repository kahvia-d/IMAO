using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Drawing;

class WindowClientSizeGetter
{
    // 引入Windows API
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
    /// 通过进程名获取窗口客户区大小（不含非客户区域）
    /// </summary>
    /// <param name="processName">进程名（不含.exe）</param>
    /// <returns>客户区大小，未找到返回Size.Empty</returns>
    public static Size GetClientSizeByProcessName(string processName)
    {
        try
        {
            var processIds = Process.GetProcessesByName(processName)
                .Select(process => (uint)process.Id)
                .ToHashSet();
            if (processIds.Count == 0)
            {
                return Size.Empty;
            }

            Size largestClientSize = Size.Empty;
            long largestClientArea = 0;

            EnumWindows((hWnd, lParam) =>
            {
                GetWindowThreadProcessId(hWnd, out uint processId);

                if (processIds.Contains(processId) && IsWindowVisible(hWnd) && !IsIconic(hWnd) &&
                    GetClientRect(hWnd, out RECT clientRect))
                {
                    int width = clientRect.Right - clientRect.Left;
                    int height = clientRect.Bottom - clientRect.Top;
                    long area = (long)width * height;
                    if (width >= 640 && height >= 360 && area > largestClientArea)
                    {
                        largestClientSize = new Size(width, height);
                        largestClientArea = area;
                    }
                }
                return true;
            }, IntPtr.Zero);

            return largestClientSize;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"GetClientSizeByProcessName: {ex.Message}");
            return Size.Empty;
        }
    }

}

class GameWindow()
{
    /// <summary>
    /// 游戏窗口现在能不能开始探索：目标进程在跑，并且有一个可见、未最小化、客户区不小于 640x360
    /// 的游戏窗口（尺寸下限与"取最大的那个窗口"都在 <see cref="WindowClientSizeGetter"/> 里）。
    ///
    /// 这里**不再看画面比例**。HUD 裁剪已经改成按游戏自己的 HUD 缩放加贴边锚定来定位
    /// （原生 <c>IMao-Core/src/Coordinate/HudLayout.h</c>：<c>scale = min(宽/1600, 高/900)</c>，
    /// 每个控件再贴上/下/左/右某一条边），16:9、16:10、21:9 走的是同一套规则，原生侧也早就
    /// 删掉了坐标识别的分辨率白名单。原来那条 <c>1.76 &lt; 宽高比 &lt; 1.78</c> 的窄带是最后一个
    /// "只有 16:9 能用" 的关卡：2560x1600、3840x2160 的玩家点「开始探索」会直接拿到
    /// <c>ExplorationToggleResult.WindowSizeRejected</c>，首页按钮、手柄和快捷键三处一样被拒。
    /// </summary>
    public static bool CheckGameWindowSize()
        => WindowClientSizeGetter.GetClientSizeByProcessName("Client-Win64-Shipping") != Size.Empty;
}
