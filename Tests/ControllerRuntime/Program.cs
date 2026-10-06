using IMao_WinUI.Services;
using IMao_WinUI.Models;
using System.Runtime.InteropServices;

static void Check(bool value, string name)
{ if (!value) throw new Exception(name + ": " + SdlNative.Error); Console.WriteLine("PASS " + name); }

Environment.SetEnvironmentVariable("SDL_JOYSTICK_ENHANCED_REPORTS", "1");
using var source = new ControllerDeviceSource();
source.GetDevices();
Check(source.Diagnostic.Length == 0, "real SDL DLL initializes alongside XInput");
Check(Marshal.PtrToStringUTF8(Native.GetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS")) == "1" &&
    Marshal.PtrToStringUTF8(Native.GetHint("SDL_JOYSTICK_ENHANCED_REPORTS")) == "0", "background input enabled without enhanced reports");
IntPtr name = Marshal.StringToCoTaskMemUTF8("IMAO controller regression fixture");
uint id = 0; IntPtr joystick = IntPtr.Zero, gamepad = IntPtr.Zero;
try
{
    var description = new Native.VirtualDesc { Version = (uint)Marshal.SizeOf<Native.VirtualDesc>(), Type = 1,
        Vendor = 0x054c, Product = 0x05c4, Axes = 6, Buttons = 15, ButtonMask = 0x7fff, AxisMask = 0x3f, Name = name };
    id = Native.Attach(ref description);
    Check(id != 0, "SDL attaches isolated virtual gamepad (no system input injection)");
    joystick = Native.OpenJoystick(id); gamepad = SdlNative.OpenGamepad(id);
    Check(joystick != IntPtr.Zero && gamepad != IntPtr.Zero, "native handles and gamepad ABI load");
    Check(Native.SetAxis(joystick, 4, -32768) && Native.SetAxis(joystick, 5, -32768), "virtual triggers start released");
    SdlNative.UpdateGamepads();
    Check(ControllerDeviceSource.ReadPlayStation(gamepad, 4).Neutral, "production reader sees neutral SDL input");
    for (int button = 0; button < 4; button++)
    {
        Check(Native.SetButton(joystick, button, true), "set virtual face button " + button);
        SdlNative.UpdateGamepads();
        Check(ControllerDeviceSource.ReadPlayStation(gamepad, 4).Buttons ==
            new[] { GamepadButtons.A, GamepadButtons.B, GamepadButtons.X, GamepadButtons.Y }[button], "production native face mapping " + button);
        Native.SetButton(joystick, button, false);
    }
    Native.SetAxis(joystick, 1, -24000); Native.SetAxis(joystick, 4, 32767);
    Native.SetButton(joystick, 9, true); Native.SetButton(joystick, 6, true);
    SdlNative.UpdateGamepads();
    var sample = ControllerDeviceSource.ReadPlayStation(gamepad, 4);
    Check(sample.LeftY == 24000 && sample.LeftTrigger == 255 && sample.Buttons == (GamepadButtons.LB | GamepadButtons.Menu),
        "native sampling maps upward stick, L2 and L1+Options together");
    Console.WriteLine("Controller native integration tests passed. USB/Bluetooth physical devices were not tested.");
}
finally
{
    if (gamepad != IntPtr.Zero) SdlNative.CloseGamepad(gamepad);
    if (joystick != IntPtr.Zero) Native.CloseJoystick(joystick);
    if (id != 0) Native.Detach(id);
    Marshal.FreeCoTaskMem(name);
}

internal static class Native
{
    [StructLayout(LayoutKind.Sequential)] internal struct VirtualDesc
    {
        public uint Version;
        public ushort Type, Padding, Vendor, Product, Axes, Buttons, Balls, Hats, Touchpads, Sensors, PaddingA, PaddingB;
        public uint ButtonMask, AxisMask;
        public IntPtr Name, TouchpadDescriptions, SensorDescriptions, Userdata, Update, SetPlayerIndex, Rumble,
            RumbleTriggers, SetLed, SendEffect, SetSensorsEnabled, Cleanup;
    }
    [DllImport("IMao.SDL3", EntryPoint="SDL_GetHint", CallingConvention=CallingConvention.Cdecl)] internal static extern IntPtr GetHint([MarshalAs(UnmanagedType.LPUTF8Str)] string name);
    [DllImport("IMao.SDL3", EntryPoint="SDL_AttachVirtualJoystick", CallingConvention=CallingConvention.Cdecl)] internal static extern uint Attach(ref VirtualDesc desc);
    [DllImport("IMao.SDL3", EntryPoint="SDL_OpenJoystick", CallingConvention=CallingConvention.Cdecl)] internal static extern IntPtr OpenJoystick(uint id);
    [DllImport("IMao.SDL3", EntryPoint="SDL_CloseJoystick", CallingConvention=CallingConvention.Cdecl)] internal static extern void CloseJoystick(IntPtr pad);
    [DllImport("IMao.SDL3", EntryPoint="SDL_DetachVirtualJoystick", CallingConvention=CallingConvention.Cdecl)] [return:MarshalAs(UnmanagedType.I1)] internal static extern bool Detach(uint id);
    [DllImport("IMao.SDL3", EntryPoint="SDL_SetJoystickVirtualAxis", CallingConvention=CallingConvention.Cdecl)] [return:MarshalAs(UnmanagedType.I1)] internal static extern bool SetAxis(IntPtr pad, int axis, short value);
    [DllImport("IMao.SDL3", EntryPoint="SDL_SetJoystickVirtualButton", CallingConvention=CallingConvention.Cdecl)] [return:MarshalAs(UnmanagedType.I1)] internal static extern bool SetButton(IntPtr pad, int button, [MarshalAs(UnmanagedType.I1)] bool value);
}
