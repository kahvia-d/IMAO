using System.Text.RegularExpressions;

namespace IMao_WinUI.Models;

public enum GamepadButtonLayout { Auto, Xbox, PlayStation }
public sealed record GamepadDeviceInfo(int Handle, string Id, string Name, GamepadButtonLayout Layout);

public interface IGamepadDeviceSource : IDisposable
{
    IReadOnlyList<GamepadDeviceInfo> GetDevices();
    GamepadSample Read(int handle);
    string Diagnostic { get; }
}

/// <summary>One selected device, with a neutral barrier across connection and configuration changes.</summary>
public sealed class GamepadDeviceSelection
{
    public int Handle { get; private set; } = -1;
    public GamepadDeviceInfo? Device { get; private set; }
    public bool WaitingForNeutral { get; private set; } = true;
    private bool connected;
    private string configuredId = "";
    private int configuredIndex = -1;

    public void Configure(string id, int legacyIndex)
    {
        configuredId = id; configuredIndex = legacyIndex;
        Device = null; Handle = -1; connected = false; WaitingForNeutral = true;
    }

    public GamepadSample Read(IGamepadDeviceSource source)
    {
        if (Device is null)
        {
            var devices = source.GetDevices();
            Device = !string.IsNullOrEmpty(configuredId)
                ? devices.FirstOrDefault(d => d.Id == configuredId)
                : configuredIndex >= 0
                    ? new(configuredIndex, $"xinput:{configuredIndex}", $"Xbox / XInput {configuredIndex + 1}", GamepadButtonLayout.Xbox)
                    : devices.FirstOrDefault(d => source.Read(d.Handle).Connected);
            Handle = Device?.Handle ?? -1;
        }
        var sample = Handle < 0 ? new(false, -1, GamepadButtons.None) : source.Read(Handle);
        if (!sample.Connected) { connected = false; WaitingForNeutral = true; return sample; }
        if (!connected) { connected = true; WaitingForNeutral = true; }
        if (WaitingForNeutral)
        {
            if (!sample.Neutral) return new(false, Handle, GamepadButtons.None);
            WaitingForNeutral = false;
        }
        return sample;
    }
}

public static class PlayStationGamepadMapping
{
    // SDL3 gamepad buttons describe physical positions, independent of printed labels.
    private static readonly GamepadButtons[] Buttons = {
        GamepadButtons.A, GamepadButtons.B, GamepadButtons.X, GamepadButtons.Y,
        GamepadButtons.View, GamepadButtons.None, GamepadButtons.Menu,
        GamepadButtons.L3, GamepadButtons.R3, GamepadButtons.LB, GamepadButtons.RB,
        GamepadButtons.Up, GamepadButtons.Down, GamepadButtons.Left, GamepadButtons.Right
    };
    public static GamepadSample Convert(int handle, uint buttons, short lx, short ly, short rx, short ry, short lt, short rt)
    {
        GamepadButtons result = GamepadButtons.None;
        for (int i = 0; i < Buttons.Length; i++) if ((buttons & (1u << i)) != 0) result |= Buttons[i];
        return new(true, handle, result, Trigger(lt), Trigger(rt), lx, Invert(ly), rx, Invert(ry));
    }
    private static short Invert(short value) => (short)Math.Clamp(-(int)value, short.MinValue, short.MaxValue);
    private static byte Trigger(short value) => (byte)Math.Clamp(((int)value * 255 + 16383) / 32767, 0, 255);
}

/// <summary>Explicit per-service formatter; no shared mutable global controller family.</summary>
public static class GamepadLabels
{
    private static readonly IReadOnlyDictionary<string, string> Ps = new Dictionary<string, string> {
        ["A"] = "×", ["B"] = "○", ["X"] = "□", ["Y"] = "△",
        ["LB"] = "L1", ["RB"] = "R1", ["LT"] = "L2", ["RT"] = "R2",
        ["LS"] = "L3", ["RS"] = "R3", ["Start"] = "Options", ["Back"] = "Share / Create"
    };
    public static string Format(string text, GamepadButtonLayout layout) => layout != GamepadButtonLayout.PlayStation
        ? text : Regex.Replace(text, @"\b(LB|RB|LT|RT|LS|RS|Start|Back|A|B|X|Y)\b", match => Ps[match.Value]);
}
