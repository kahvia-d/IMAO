using System.Text.Json;
using IMao_WinUI.Models;
int failures = 0;
void Check(bool value, string why) { if (!value) { Console.Error.WriteLine("FAIL " + why); failures++; } }
Check(new CaptureArea(20, 80, 1300, 800).Fits(1920, 1032), "windowed crop");
Check(new CaptureArea(0, 0, 1920, 1080).Fits(1920, 1080), "fullscreen");
Check(!new CaptureArea(-1, 0, 1000, 720).Fits(1920, 1080), "negative crop");
Check(!new CaptureArea(0, 0, 639, 360).Fits(1920, 1080), "undersized crop");
Check(!new CaptureArea(0, 0, 1921, 1080).Fits(1920, 1080), "outside client");
var selected = new BrowserCaptureSelection(9007199254740993, 42, 133713371337133713);
var json = JsonSerializer.SerializeToElement(selected.ToPayload());
Check(json.GetProperty("hwnd").GetString() == "9007199254740993", "HWND avoids double precision loss");
Check(json.GetProperty("processCreatedAt").GetString() == "133713371337133713", "process time exact");
Check(!json.TryGetProperty("area", out _), "player does not supply or persist viewport geometry");
try { (selected with { Window = 0 }).ToPayload(); Check(false, "zero handle rejected"); } catch (ArgumentException) { }
Console.WriteLine($"Cloud capture managed failures={failures}");return failures == 0 ? 0 : 1;
