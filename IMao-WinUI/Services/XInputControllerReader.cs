using IMao_WinUI.Models;
using System.Runtime.InteropServices;

namespace IMao_WinUI.Services;

// One reader for the whole app. No vibration, remapping or keyboard injection.
internal sealed class XInputControllerReader
{
    private delegate uint ReadState(uint index, out NativeState state);
    private ReadState? read;
    private bool initialized;
    [StructLayout(LayoutKind.Sequential)]
    private struct NativeState
    {
        public uint Packet;
        public ushort Buttons;
        public byte LeftTrigger, RightTrigger;
        public short LeftX, LeftY, RightX, RightY;
    }
    [DllImport("xinput1_4.dll", EntryPoint = "XInputGetState")]
    private static extern uint Read14(uint index, out NativeState state);
    [DllImport("xinput1_3.dll", EntryPoint = "XInputGetState")]
    private static extern uint Read13(uint index, out NativeState state);
    [DllImport("xinput9_1_0.dll", EntryPoint = "XInputGetState")]
    private static extern uint Read910(uint index, out NativeState state);

    public GamepadSample Read(int index)
    {
        if (!initialized)
        {
            initialized = true;
            var unavailable = new List<string>(3);
            foreach (var (name, candidate) in new (string, ReadState)[] { ("xinput1_4.dll", Read14), ("xinput1_3.dll", Read13), ("xinput9_1_0.dll", Read910) })
            {
                try { candidate(0, out _); read = candidate; break; }
                catch (DllNotFoundException) { unavailable.Add(name); }
                catch (EntryPointNotFoundException) { unavailable.Add(name); }
            }
            if (read is null) Diagnostic = "XInput 组件不可用（" + string.Join(" / ", unavailable) + " 都无法加载）";
        }
        if (read is null || index is < 0 or > 3 || read((uint)index, out var s) != 0)
            return new(false, index, GamepadButtons.None);
        return new(true, index, (GamepadButtons)s.Buttons, s.LeftTrigger, s.RightTrigger,
            s.LeftX, s.LeftY, s.RightX, s.RightY);
    }

    /// <summary>
    /// Why XInput could not be used at all, or empty while some provider loaded.
    /// </summary>
    /// <remarks>
    /// Three DLL names are tried because a machine may carry any one of them and no other, and until 2026-10 a
    /// machine carrying none looked exactly like a machine with no controller plugged in: both produced the same
    /// sentence and nothing else. That distinction is the whole diagnosis on a compatibility layer, where "the
    /// layer exposes no XInput" and "Steam Input is presenting nothing to it" have entirely different fixes, so
    /// the failure is now named instead of being swallowed by an empty result.
    /// </remarks>
    public string Diagnostic { get; private set; } = "";
}
