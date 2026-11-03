using IMao_WinUI.Helpers;
using IMao_WinUI.Models;
using IMao_WinUI.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Storage.Streams;

namespace IMao_WinUI.Views;

internal static class CaptureWindowDialog
{
    public static async Task ShowAsync(XamlRoot root, CoreHostService core)
    {
        var windows = new ComboBox { Header = "云鸣潮所在的浏览器窗口", HorizontalAlignment = HorizontalAlignment.Stretch };
        var preview = new Image { MaxHeight = 330, Stretch = Stretch.Uniform };
        var message = new TextBlock { Text = "请在浏览器中打开云鸣潮游戏画面，再选择窗口。全屏和普通窗口均可自动识别游戏区域。", TextWrapping = TextWrapping.Wrap };
        var refresh = new Button { Content = "刷新窗口列表" };
        var content = new StackPanel { Spacing = 12, Width = 680 };
        content.Children.Add(message);content.Children.Add(windows);content.Children.Add(refresh);content.Children.Add(preview);
        var dialog = new ContentDialog
        {
            XamlRoot = root, Title = "选择游戏窗口", Content = content,
            PrimaryButtonText = "使用此窗口", SecondaryButtonText = "使用本地客户端", CloseButtonText = "取消",
            IsPrimaryButtonEnabled = false
        };
        dialog.Resources["ContentDialogMaxWidth"] = 760d;
        BrowserCaptureSelection? selected = null;
        int generation = 0;
        bool closed = false;
        void Reload() { generation++; selected = null; preview.Source = null;dialog.IsPrimaryButtonEnabled = false;windows.ItemsSource = BrowserWindows.Enumerate(); }
        refresh.Click += (_, _) => Reload();
        windows.SelectionChanged += async (_, _) =>
        {
            int current = ++generation; selected = null; preview.Source = null; dialog.IsPrimaryButtonEnabled = false;
            if (windows.SelectedItem is not BrowserWindows.Entry entry) return;
            message.Text = "正在自动识别游戏画面区域…";
            try
            {
                var data = await core.ExecuteMarkerAsync("markerCaptureWindowPreview", new { captureTarget = entry.Selection.ToPayload() });
                if (closed || current != generation) return;
                byte[] png = Convert.FromBase64String(data.GetProperty("png").GetString()!);
                using var stream = new InMemoryRandomAccessStream();
                using (var writer = new DataWriter(stream.GetOutputStreamAt(0))) { writer.WriteBytes(png); await writer.StoreAsync(); }
                stream.Seek(0);
                var bitmap = new BitmapImage(); await bitmap.SetSourceAsync(stream);
                if (closed || current != generation) return;
                preview.Source = bitmap;selected = entry.Selection;dialog.IsPrimaryButtonEnabled = true;
                message.Text = $"已自动识别 {data.GetProperty("width").GetInt32()} × {data.GetProperty("height").GetInt32()} 游戏区域。窗口缩放或切换全屏后会自动重新识别，无需框选。";
            }
            catch (Exception error) { if (!closed && current == generation) message.Text = "预览暂不可用：" + error.Message; }
        };
        Reload();
        var result = await dialog.ShowAsync();closed = true;generation++;
        if (result == ContentDialogResult.Primary && selected is not null) core.SelectCaptureTarget(selected);
        else if (result == ContentDialogResult.Secondary) core.SelectCaptureTarget(null);
    }
}
