# Isolates which window style makes the overlay click-through.
#
# The composition overlay swallows the player's clicks while the colorkey overlay does not, and both
# carry WS_EX_TRANSPARENT, so the difference has to be one of the other style bits. This creates the
# same window with one style variation at a time, brings the game to the foreground, and asks Windows
# which window a click at the game's centre would reach.
#
# Run it from an elevated shell with the game running and in the foreground. Nothing here is part of
# the program; it exists to name the bit before any of it is changed.
[CmdletBinding()]
param(
    [string]$ProcessName = 'Client-Win64-Shipping'
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class HitProbe
{
    public const int GWL_EXSTYLE = -20;
    public const int WS_EX_TOPMOST = 0x00000008;
    public const int WS_EX_TRANSPARENT = 0x00000020;
    public const int WS_EX_LAYERED = 0x00080000;
    public const int WS_EX_NOACTIVATE = 0x08000000;
    public const int WS_EX_TOOLWINDOW = 0x00000080;
    public const int WS_EX_NOREDIRECTIONBITMAP = 0x00200000;
    public const int SW_SHOWNOACTIVATE = 4;
    public static readonly IntPtr HWND_TOPMOST = new IntPtr(-1);
    public const uint SWP_NOACTIVATE = 0x0010;
    public const uint SWP_SHOWWINDOW = 0x0040;

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int left, top, right, bottom; }
    // POINT is defined here rather than taken from System.Drawing: the assembly that carries it is
    // forwarded in a way Add-Type does not resolve without an extra reference.
    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int X, Y; public POINT(int x, int y) { X = x; Y = y; } }
    public delegate bool EnumProc(IntPtr hwnd, IntPtr param);

    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr param);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr hwnd, StringBuilder text, int count);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hwnd, ref POINT point);
    [DllImport("user32.dll")] public static extern IntPtr CreateWindowEx(int exStyle, string cls, string title, int style,
        int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr instance, IntPtr param);
    [DllImport("user32.dll")] public static extern bool DestroyWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hwnd, int command);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int w, int h, uint flags);
    [DllImport("user32.dll")] public static extern bool UpdateWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT point);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern IntPtr GetDesktopWindow();
}
'@

function New-HitProbeClass {
    # A window class is created once for the process, shared by every variant.
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class ProbeClass
{
    [StructLayout(LayoutKind.Sequential)]
    public struct WNDCLASSEX
    {
        public uint cbSize; public uint style; public IntPtr lpfnWndProc;
        public int cbClsExtra; public int cbWndExtra; public IntPtr hInstance;
        public IntPtr hIcon; public IntPtr hCursor; public IntPtr hbrBackground;
        public string lpszMenuName; public string lpszClassName; public IntPtr hIconSm;
    }
    public delegate IntPtr WndProc(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern ushort RegisterClassEx(ref WNDCLASSEX description);
    [DllImport("user32.dll")] public static extern IntPtr DefWindowProc(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
    [DllImport("kernel32.dll")] public static extern IntPtr GetModuleHandle(string name);
    public static WndProc KeepAlive; // The delegate must outlive registration or the class dies with it.
    public static void Register()
    {
        KeepAlive = (hwnd, msg, w, l) => DefWindowProc(hwnd, msg, w, l);
        var description = new WNDCLASSEX();
        description.cbSize = (uint)Marshal.SizeOf(description);
        description.lpfnWndProc = Marshal.GetFunctionPointerForDelegate(KeepAlive);
        description.hInstance = GetModuleHandle(null);
        description.lpszClassName = "IMaoHitProbeClass";
        RegisterClassEx(ref description);
    }
}
'@
}

$game = Get-Process -Name $ProcessName -ErrorAction Stop | Select-Object -First 1
$gameHwnd = $game.MainWindowHandle
if ($gameHwnd -eq [IntPtr]::Zero) { throw 'The game has no main window; bring it up first.' }

$client = New-Object HitProbe+RECT
[void][HitProbe]::GetClientRect($gameHwnd, [ref]$client)
$origin = New-Object HitProbe+POINT 0, 0
[void][HitProbe]::ClientToScreen($gameHwnd, [ref]$origin)
$width = $client.right
$height = $client.bottom
$centre = New-Object HitProbe+POINT (($origin.X + [int]($width / 2))), (($origin.Y + [int]($height / 2)))

# Focus matters: the overlay's click path ignores clicks while the game is not in front, so a
# measurement taken with something else focused would describe a situation the player never sees.
[void][HitProbe]::SetForegroundWindow($gameHwnd)
Start-Sleep -Milliseconds 700
$foreground = [HitProbe]::GetForegroundWindow()

Write-Host ("game hwnd={0} at {1},{2} {3}x{4}; probing {5},{6}" -f $gameHwnd, $origin.X, $origin.Y, $width, $height, $centre.X, $centre.Y)
Write-Host ("foreground after focusing the game: {0} ({1})" -f $foreground, $(if ($foreground -eq $gameHwnd) { 'the game' } else { 'NOT the game' }))
Write-Host ''

New-HitProbeClass
[ProbeClass]::Register()

$base = [HitProbe]::WS_EX_TOPMOST -bor [HitProbe]::WS_EX_TRANSPARENT -bor [HitProbe]::WS_EX_NOACTIVATE -bor [HitProbe]::WS_EX_TOOLWINDOW
$variants = @(
    [pscustomobject]@{ Name = 'layered + transparent (today)'; Style = $base -bor [HitProbe]::WS_EX_LAYERED }
    [pscustomobject]@{ Name = 'noredirection + transparent (composition)'; Style = $base -bor [HitProbe]::WS_EX_NOREDIRECTIONBITMAP }
    [pscustomobject]@{ Name = 'layered + noredirection + transparent'; Style = $base -bor [HitProbe]::WS_EX_LAYERED -bor [HitProbe]::WS_EX_NOREDIRECTIONBITMAP }
    [pscustomobject]@{ Name = 'noredirection, no transparent'; Style = $base -bor [HitProbe]::WS_EX_NOREDIRECTIONBITMAP -bxor [HitProbe]::WS_EX_TRANSPARENT }
    [pscustomobject]@{ Name = 'transparent only'; Style = $base -bxor [HitProbe]::WS_EX_TOPMOST }
)

foreach ($variant in $variants) {
    $window = [HitProbe]::CreateWindowEx($variant.Style, 'IMaoHitProbeClass', 'IMao hit probe', 0x80000000,
        $origin.X, $origin.Y, $width, $height, [IntPtr]::Zero, [IntPtr]::Zero, [IntPtr]::Zero, [IntPtr]::Zero)
    if ($window -eq [IntPtr]::Zero) { Write-Host ("  {0,-42} CreateWindowEx failed" -f $variant.Name); continue }
    [void][HitProbe]::ShowWindow($window, [HitProbe]::SW_SHOWNOACTIVATE)
    [void][HitProbe]::SetWindowPos($window, [HitProbe]::HWND_TOPMOST, $origin.X, $origin.Y, $width, $height,
        [HitProbe]::SWP_NOACTIVATE -bor [HitProbe]::SWP_SHOWWINDOW)
    [void][HitProbe]::UpdateWindow($window)
    Start-Sleep -Milliseconds 350

    $hit = [HitProbe]::WindowFromPoint($centre)
    $verdict = if ($hit -eq $window) { 'SWALLOWS the click' } elseif ($hit -eq $gameHwnd) { 'game receives it' } else { "other hwnd $hit" }
    Write-Host ("  {0,-42} {1}" -f $variant.Name, $verdict)

    [void][HitProbe]::DestroyWindow($window)
    Start-Sleep -Milliseconds 200
}

Write-Host ''
Write-Host 'The overlay needs "game receives it" in the composition variant too.' -ForegroundColor Yellow
