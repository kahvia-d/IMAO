using IMao_WinUI.Models;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;

namespace IMao_WinUI.Views;

public sealed partial class SettingsPage
{
    private bool restoringOverlayScales = true;
    private bool savingOverlayScales;
    private readonly Dictionary<string, int> pendingOverlayScales = new();

    private (string Key, Slider Slider, TextBlock Label, int Value)[] OverlayScaleRows(RuntimeConfiguration value) =>
    [
        ("MinimapStatus", MinimapStatusScaleSlider, MinimapStatusScaleLabel, value.MinimapStatusScalePercent),
        ("MapStatus", MapStatusScaleSlider, MapStatusScaleLabel, value.MapStatusScalePercent),
        ("Tools", ToolsScaleSlider, ToolsScaleLabel, value.ToolsScalePercent),
        ("Guide", GuideScaleSlider, GuideScaleLabel, value.GuideScalePercent),
        ("Hint", HintScaleSlider, HintScaleLabel, value.HintScalePercent),
        ("MapMarker", MapMarkerScaleSlider, MapMarkerScaleLabel, value.MapMarkerScalePercent),
        ("MinimapMarker", MinimapMarkerScaleSlider, MinimapMarkerScaleLabel, value.MinimapMarkerScalePercent),
        ("HandPalette", HandPaletteScaleSlider, HandPaletteScaleLabel, value.HandPaletteScalePercent),
        ("HandNode", HandNodeScaleSlider, HandNodeScaleLabel, value.HandNodeScalePercent),
        ("StatusBall", StatusBallScaleSlider, StatusBallScaleLabel, value.StatusBallScalePercent),
    ];

    private void RestoreOverlayScales()
    {
        // Configuration/status events may arrive while an earlier slider value is in flight.
        // Keep the local intent visible until the serialized queue has drained.
        if (savingOverlayScales) return;
        restoringOverlayScales = true;
        try
        {
            foreach (var row in OverlayScaleRows(coreHost.Configuration))
            {
                row.Slider.Value = row.Value;
                row.Label.Text = ScaleLabel(row.Value);
            }
        }
        finally { restoringOverlayScales = false; }
    }

    private static string ScaleLabel(int percent) => percent == 100 ? "100% · 自动适配" : $"{percent}%";

    private async void OverlayScale_ValueChanged(object sender, RangeBaseValueChangedEventArgs e)
    {
        if (restoringOverlayScales || !IsLoaded || sender is not Slider slider || slider.Tag is not string key) return;
        int value = (int)Math.Round(e.NewValue);
        foreach (var row in OverlayScaleRows(coreHost.Configuration))
            if (row.Key == key) row.Label.Text = ScaleLabel(value);
        pendingOverlayScales[key] = value;
        await SaveOverlayScalesAsync();
    }

    private async Task SaveOverlayScalesAsync()
    {
        if (savingOverlayScales) return;
        savingOverlayScales = true;
        try
        {
            while (pendingOverlayScales.Count != 0)
            {
                var batch = new Dictionary<string, int>(pendingOverlayScales);
                pendingOverlayScales.Clear();
                int? Pending(string key) => batch.TryGetValue(key, out int value) ? value : null;
                bool applied = await coreHost.ConfigureAsync(
                    minimapStatusScalePercent: Pending("MinimapStatus"),
                    mapStatusScalePercent: Pending("MapStatus"),
                    toolsScalePercent: Pending("Tools"),
                    guideScalePercent: Pending("Guide"),
                    hintScalePercent: Pending("Hint"),
                    mapMarkerScalePercent: Pending("MapMarker"),
                    minimapMarkerScalePercent: Pending("MinimapMarker"),
                    handPaletteScalePercent: Pending("HandPalette"),
                    handNodeScalePercent: Pending("HandNode"),
                    statusBallScalePercent: Pending("StatusBall"));
                OverlayScaleMessage.Severity = InfoBarSeverity.Warning;
                OverlayScaleMessage.Message = applied ? "" : "设置状态：" + coreHost.LastFault;
                OverlayScaleMessage.IsOpen = !applied;
            }
        }
        catch (Exception error)
        {
            OverlayScaleMessage.Severity = InfoBarSeverity.Error;
            OverlayScaleMessage.Message = error.Message;
            OverlayScaleMessage.IsOpen = true;
        }
        finally
        {
            savingOverlayScales = false;
            RestoreOverlayScales();
        }
    }
}
