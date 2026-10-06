using IMao_WinUI.Models;
using System.Text.Json;

internal static class PlayStationInputTests
{
    public static void Run(Action<bool, string> check)
    {
        using var config = JsonDocument.Parse(JsonSerializer.Serialize(new RuntimeConfiguration()));
        check(config.RootElement.TryGetProperty("GamepadButtonLayout", out _) &&
            config.RootElement.TryGetProperty("GamepadDeviceId", out _),
            "controller configuration persists native device identity and button layout");
        for (int i = 0; i < 4; i++)
        {
            var expected = new[] { GamepadButtons.A, GamepadButtons.B, GamepadButtons.X, GamepadButtons.Y }[i];
            check(PlayStationGamepadMapping.Convert(4, 1u << i, 0, 0, 0, 0, 0, 0).Buttons == expected,
                $"PS physical face position {i} maps to {expected}");
        }
        var all = PlayStationGamepadMapping.Convert(4, 0x7fff, -32768, -32768, 32767, 32767, 32767, -32768);
        check(all.Buttons == (GamepadButtons)0xf3ff && all.LeftY == 32767 && all.RightY == -32767 &&
            all.LeftX == -32768 && all.RightX == 32767 && all.LeftTrigger == 255 && all.RightTrigger == 0,
            "PS shoulders, sticks, menu, dpad, trigger ranges and Y inversion preserve logical input");
        check(PlayStationGamepadMapping.Convert(4, 0, 11999, 0, 0, 0, 0, 0).Neutral &&
            !PlayStationGamepadMapping.Convert(4, 0, 12000, 0, 0, 0, 0, 0).Neutral,
            "PS input shares existing dead zone and neutral threshold");
        check(GamepadLabels.Format("LB+B / LB+X / LB+Start / LS / A / Y", GamepadButtonLayout.PlayStation) ==
            "L1+○ / L1+□ / L1+Options / L3 / × / △", "PS hints translate full chords by position");
        check(GamepadLabels.Format("Xbox / LB+B / MAX", GamepadButtonLayout.Xbox) == "Xbox / LB+B / MAX" &&
            GamepadLabels.Format("Xbox / MAX", GamepadButtonLayout.PlayStation) == "Xbox / MAX",
            "Xbox hints and words containing button letters remain unchanged");
        var source = new Source();
        var selection = new GamepadDeviceSelection();
        source.Samples[0] = new(true, 0, GamepadButtons.A);
        selection.Configure("", -1);
        check(!selection.Read(source).Connected && selection.Handle == 0, "auto selection prefers XInput and blocks held entry");
        source.Samples[0] = new(true, 0, GamepadButtons.None);
        check(selection.Read(source).Connected, "neutral sample unlocks selected controller");
        source.Samples[0] = new(false, 0, GamepadButtons.None);
        check(!selection.Read(source).Connected && selection.Handle == 0, "disconnect cannot take over another connected PS controller");
        source.Samples[0] = new(true, 0, GamepadButtons.LB | GamepadButtons.Menu);
        check(!selection.Read(source).Connected, "reconnected held exploration chord remains blocked");
        source.Samples[0] = new(true, 0, GamepadButtons.None);
        selection.Read(source);
        source.Samples[0] = new(true, 0, GamepadButtons.B);
        check(selection.Read(source).Buttons == GamepadButtons.B, "reconnection resumes only after full neutral");
        selection.Configure("ps:test", -1);
        source.Samples[4] = new(true, 4, GamepadButtons.None, LeftTrigger: 255);
        check(!selection.Read(source).Connected && selection.Handle == 4, "explicit PS identity ignores Xbox and held trigger");
        source.Samples[4] = new(true, 4, GamepadButtons.None);
        check(selection.Read(source).Connected && selection.Device?.Layout == GamepadButtonLayout.PlayStation,
            "native device family controls automatic hints");
        selection.Configure("ps:missing", -1);
        check(!selection.Read(source).Connected && selection.Handle == -1, "missing configured device never falls back to another pad");
        source.Samples[0] = new(false, 0, GamepadButtons.None);
        selection.Configure("", -1);
        check(selection.Read(source).DeviceId == 4, "auto selection discovers native PS when XInput is absent");
        var nativeConfig = new RuntimeConfiguration { GamepadDeviceId = "ps:test", GamepadButtonLayout = GamepadButtonLayout.PlayStation };
        nativeConfig.Validate();
        check(JsonSerializer.Deserialize<RuntimeConfiguration>(JsonSerializer.Serialize(nativeConfig)) == nativeConfig,
            "native controller settings round trip without changing existing defaults");
        var map = new GamepadInputInterpreter();
        var world = new GamepadInputContext(GamepadInputMode.Gameplay, "ps:world", true);
        var neutral = PlayStationGamepadMapping.Convert(4, 0, 0, 0, 0, 0, 0, 0);
        map.Update(neutral, world, 0);
        map.Update(PlayStationGamepadMapping.Convert(4, (1u << 9) | (1u << 1), 0, 0, 0, 0, 0, 0), world, 16);
        check(map.Update(neutral, world, 32).Action == GamepadAction.CompleteCurrent,
            "native L1+circle reaches existing completion chord exactly once on release");
        check(map.Update(neutral, world, 48).Action is null, "PS completion chord cannot double fire");
    }

    private sealed class Source : IGamepadDeviceSource
    {
        public string Diagnostic => "";
        public Dictionary<int, GamepadSample> Samples = new() { [0] = new(false, 0, GamepadButtons.None), [4] = new(true, 4, GamepadButtons.None) };
        public IReadOnlyList<GamepadDeviceInfo> GetDevices() => new[] {
            new GamepadDeviceInfo(0, "xinput:0", "Xbox", GamepadButtonLayout.Xbox),
            new GamepadDeviceInfo(4, "ps:test", "DualSense", GamepadButtonLayout.PlayStation) };
        public GamepadSample Read(int handle) => Samples.GetValueOrDefault(handle, new(false, handle, GamepadButtons.None));
        public void Dispose() { }
    }
}
