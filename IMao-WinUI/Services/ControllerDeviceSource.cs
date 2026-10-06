using IMao_WinUI.Models;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

namespace IMao_WinUI.Services;

internal sealed class DelegateControllerDeviceSource(Func<int, GamepadSample> read) : IGamepadDeviceSource
{
    public string Diagnostic => "";
    public IReadOnlyList<GamepadDeviceInfo> GetDevices() => Enumerable.Range(0, 4)
        .Select(i => new GamepadDeviceInfo(i, $"xinput:{i}", $"Xbox / XInput {i + 1}", GamepadButtonLayout.Xbox)).ToArray();
    public GamepadSample Read(int handle) => read(handle);
    public void Dispose() { }
}

/// <summary>XInput retains slots 0-3. Native PS handles are stable for this source's lifetime.</summary>
internal sealed class ControllerDeviceSource : IGamepadDeviceSource
{
    private sealed class PsDevice(GamepadDeviceInfo info)
    {
        public GamepadDeviceInfo Info = info;
        public IntPtr Controller;
        public uint Instance;
    }
    private readonly XInputControllerReader xbox = new();
    private readonly Dictionary<string, PsDevice> ps = new(StringComparer.Ordinal);
    private bool attempted, initialized, disposed;
    private long enumeratedAt;
    public string Diagnostic { get; private set; } = "";

    private void EnsureInitialized()
    {
        if (attempted || disposed) return;
        attempted = true;
        try
        {
            SdlNative.InstallResolver();
            SdlNative.SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
            SdlNative.SetHint("SDL_JOYSTICK_HIDAPI_PS4", "1");
            SdlNative.SetHint("SDL_JOYSTICK_HIDAPI_PS5", "1");
            // Never switch a Bluetooth pad into a mode that changes another app's DirectInput.
            SdlNative.SetHint("SDL_JOYSTICK_ENHANCED_REPORTS", "0");
            SdlNative.SetHint("SDL_JOYSTICK_HIDAPI_PS4_RUMBLE", "0");
            SdlNative.SetHint("SDL_JOYSTICK_HIDAPI_PS5_RUMBLE", "0");
            initialized = SdlNative.InitSubSystem(0x2000);
            if (!initialized) Diagnostic = "原生 PS 手柄初始化失败：" + SdlNative.Error;
        }
        catch (Exception e) when (e is DllNotFoundException or BadImageFormatException or EntryPointNotFoundException or FileLoadException)
        { Diagnostic = "原生 PS 手柄组件无法加载，Xbox 输入仍可使用：" + e.GetType().Name; }
    }

    public IReadOnlyList<GamepadDeviceInfo> GetDevices()
    {
        Refresh();
        var result = new List<GamepadDeviceInfo>();
        for (int i = 0; i < 4; i++) if (xbox.Read(i).Connected)
            result.Add(new(i, $"xinput:{i}", $"Xbox / XInput {i + 1}", GamepadButtonLayout.Xbox));
        result.AddRange(ps.Values.Where(d => d.Controller != IntPtr.Zero).OrderBy(d => d.Info.Id).Select(d => d.Info));
        return result;
    }

    private void Refresh()
    {
        EnsureInitialized();
        if (!initialized || disposed) return;
        SdlNative.UpdateGamepads();
        if (Environment.TickCount64 - enumeratedAt < 1000) return;
        enumeratedAt = Environment.TickCount64;
        var ids = SdlNative.GetGamepads(out int count);
        try
        {
            var seen = new HashSet<uint>();
            for (int i = 0; i < count; i++)
            {
                uint instance = unchecked((uint)Marshal.ReadInt32(ids, i * 4));
                if (SdlNative.GetGamepadTypeForId(instance) is not (5 or 6)) continue;
                seen.Add(instance);
                if (ps.Values.Any(d => d.Instance == instance && d.Controller != IntPtr.Zero)) continue;
                string path = Marshal.PtrToStringUTF8(SdlNative.GetGamepadPathForId(instance)) ?? "";
                if (path.Length == 0) { Diagnostic = "PS 手柄未提供稳定设备路径，暂不自动接管"; continue; }
                string id = "ps:" + Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(path))).ToLowerInvariant();
                if (!ps.TryGetValue(id, out var device))
                {
                    string name = Marshal.PtrToStringUTF8(SdlNative.GetGamepadNameForId(instance)) ?? "PlayStation";
                    device = new(new(4 + ps.Count, id, name, GamepadButtonLayout.PlayStation));
                    ps.Add(id, device);
                }
                if (device.Controller != IntPtr.Zero) SdlNative.CloseGamepad(device.Controller);
                device.Controller = SdlNative.OpenGamepad(instance); device.Instance = instance;
                if (device.Controller == IntPtr.Zero) Diagnostic = "无法读取 PS 手柄：" + SdlNative.Error;
            }
            foreach (var device in ps.Values.Where(d => d.Controller != IntPtr.Zero && !seen.Contains(d.Instance)))
            { SdlNative.CloseGamepad(device.Controller); device.Controller = IntPtr.Zero; }
        }
        finally { if (ids != IntPtr.Zero) SdlNative.Free(ids); }
    }

    public GamepadSample Read(int handle)
    {
        if (disposed) return new(false, handle, GamepadButtons.None);
        if (handle is >= 0 and < 4) return xbox.Read(handle);
        Refresh();
        var device = ps.Values.FirstOrDefault(d => d.Info.Handle == handle);
        if (device is null || device.Controller == IntPtr.Zero || !SdlNative.GamepadConnected(device.Controller))
            return new(false, handle, GamepadButtons.None);
        return ReadPlayStation(device.Controller, handle);
    }

    internal static GamepadSample ReadPlayStation(IntPtr controller, int handle)
    {
        uint buttons = 0;
        for (int b = 0; b < 15; b++) if (SdlNative.GetGamepadButton(controller, b)) buttons |= 1u << b;
        short Axis(int axis) => SdlNative.GetGamepadAxis(controller, axis);
        return PlayStationGamepadMapping.Convert(handle, buttons, Axis(0), Axis(1), Axis(2), Axis(3), Axis(4), Axis(5));
    }

    public void Dispose()
    {
        if (disposed) return;
        disposed = true;
        if (!initialized) return;
        foreach (var device in ps.Values) if (device.Controller != IntPtr.Zero) SdlNative.CloseGamepad(device.Controller);
        ps.Clear(); SdlNative.QuitSubSystem(0x2000);
    }
}

internal static class SdlNative
{
    private const string Library = "IMao.SDL3";
    private static readonly object ResolverLock = new();
    private static bool installed;
    public static void InstallResolver()
    {
        lock (ResolverLock)
        {
            if (installed) return;
            NativeLibrary.SetDllImportResolver(typeof(SdlNative).Assembly, (name, assembly, search) =>
                name == Library ? NativeLibrary.Load(Path.Combine(AppContext.BaseDirectory, "SDL3.dll")) : IntPtr.Zero);
            installed = true;
        }
    }
    public static string Error => Marshal.PtrToStringUTF8(GetError()) ?? "未知错误";
    [DllImport(Library, EntryPoint = "SDL_SetHintWithPriority", CallingConvention = CallingConvention.Cdecl)]
    [return: MarshalAs(UnmanagedType.I1)] public static extern bool SetHint([MarshalAs(UnmanagedType.LPUTF8Str)] string name, [MarshalAs(UnmanagedType.LPUTF8Str)] string value, int priority = 2);
    [DllImport(Library, EntryPoint = "SDL_InitSubSystem", CallingConvention = CallingConvention.Cdecl)]
    [return: MarshalAs(UnmanagedType.I1)] public static extern bool InitSubSystem(uint flags);
    [DllImport(Library, EntryPoint = "SDL_QuitSubSystem", CallingConvention = CallingConvention.Cdecl)] public static extern void QuitSubSystem(uint flags);
    [DllImport(Library, EntryPoint = "SDL_GetError", CallingConvention = CallingConvention.Cdecl)] private static extern IntPtr GetError();
    [DllImport(Library, EntryPoint = "SDL_UpdateGamepads", CallingConvention = CallingConvention.Cdecl)] public static extern void UpdateGamepads();
    [DllImport(Library, EntryPoint = "SDL_GetGamepads", CallingConvention = CallingConvention.Cdecl)] public static extern IntPtr GetGamepads(out int count);
    [DllImport(Library, EntryPoint = "SDL_free", CallingConvention = CallingConvention.Cdecl)] public static extern void Free(IntPtr pointer);
    [DllImport(Library, EntryPoint = "SDL_GetGamepadTypeForID", CallingConvention = CallingConvention.Cdecl)] public static extern int GetGamepadTypeForId(uint id);
    [DllImport(Library, EntryPoint = "SDL_GetGamepadPathForID", CallingConvention = CallingConvention.Cdecl)] public static extern IntPtr GetGamepadPathForId(uint id);
    [DllImport(Library, EntryPoint = "SDL_GetGamepadNameForID", CallingConvention = CallingConvention.Cdecl)] public static extern IntPtr GetGamepadNameForId(uint id);
    [DllImport(Library, EntryPoint = "SDL_OpenGamepad", CallingConvention = CallingConvention.Cdecl)] public static extern IntPtr OpenGamepad(uint id);
    [DllImport(Library, EntryPoint = "SDL_CloseGamepad", CallingConvention = CallingConvention.Cdecl)] public static extern void CloseGamepad(IntPtr pad);
    [DllImport(Library, EntryPoint = "SDL_GamepadConnected", CallingConvention = CallingConvention.Cdecl)]
    [return: MarshalAs(UnmanagedType.I1)] public static extern bool GamepadConnected(IntPtr pad);
    [DllImport(Library, EntryPoint = "SDL_GetGamepadButton", CallingConvention = CallingConvention.Cdecl)]
    [return: MarshalAs(UnmanagedType.I1)] public static extern bool GetGamepadButton(IntPtr pad, int button);
    [DllImport(Library, EntryPoint = "SDL_GetGamepadAxis", CallingConvention = CallingConvention.Cdecl)] public static extern short GetGamepadAxis(IntPtr pad, int axis);
}
