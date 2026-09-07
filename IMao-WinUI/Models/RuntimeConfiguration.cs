namespace IMao_WinUI.Models;

public sealed record RuntimeConfiguration
{
    public int CaptureWay { get; init; }
    public int MapUpdateCycle { get; init; } = 80;
    public int MinMapUpdateCycle { get; init; } = 80;
    public bool MapEnabled { get; init; } = true;
    public bool MinMapEnabled { get; init; } = true;
    public bool SavedPointsEnabled { get; init; } = true;
    public bool StatusBarEnabled { get; init; } = true;

    public void Validate()
    {
        if (CaptureWay is < 0 or > 1) throw new ArgumentException("截图方式无效");
        if (MapUpdateCycle is < 16 or > 1000 || MinMapUpdateCycle is < 16 or > 1000)
            throw new ArgumentException("刷新间隔必须在 16–1000 毫秒之间");
    }

    internal Dictionary<string, object?> ToPayload() => new()
    {
        ["captureWay"] = CaptureWay, ["mapUpdateCycle"] = MapUpdateCycle,
        ["minMapUpdateCycle"] = MinMapUpdateCycle, ["mapEnabled"] = MapEnabled,
        ["minMapEnabled"] = MinMapEnabled, ["savedPointsEnabled"] = SavedPointsEnabled,
        ["statusBarEnabled"] = StatusBarEnabled
    };
}
