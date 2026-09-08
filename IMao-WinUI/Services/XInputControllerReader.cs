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
            foreach (ReadState candidate in new ReadState[] { Read14, Read13, Read910 })
            {
                try { candidate(0, out _); read = candidate; break; }
                catch (DllNotFoundException) { }
                catch (EntryPointNotFoundException) { }
            }
        }
        if (read is null || index is < 0 or > 3 || read((uint)index, out var s) != 0)
            return new(false, index, GamepadButtons.None);
        return new(true, index, (GamepadButtons)s.Buttons, s.LeftTrigger, s.RightTrigger,
            s.LeftX, s.LeftY, s.RightX, s.RightY);
    }
}
