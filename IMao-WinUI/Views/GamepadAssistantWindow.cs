using IMao_WinUI.Models;
using IMao_WinUI.Services;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using System.Runtime.InteropServices;
using Windows.Graphics;

namespace IMao_WinUI.Views;

internal sealed record GamepadAssistantEntry(string Label, MarkerSelection? Selection = null, string Action = "", bool Enabled = true);

// Input sampling belongs to GamepadInputService. This window only presents semantic choices.
public sealed class GamepadAssistantWindow : Window
{
    private readonly Func<GamepadAction, Task> dispatch;
    private readonly TextBlock heading = new() { FontSize = 25, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold };
    private readonly TextBlock message = new() { TextWrapping = TextWrapping.Wrap, FontSize = 14, Opacity = 0.8 };
    private readonly TextBlock hint = new() { TextWrapping = TextWrapping.Wrap, FontSize = 13, Opacity = 0.8 };
    private readonly ListView choices = new() { SelectionMode = ListViewSelectionMode.Single, IsItemClickEnabled = true };
    private readonly Button route = new() { Content = "Y · 路线操作" };
    private readonly Button back = new() { Content = "B · 返回" };
    private bool rendering;
    private readonly GamepadWindowChrome chrome;

    public bool IsClosed { get; private set; }
    internal GamepadAssistantEntry? SelectedEntry => (choices.SelectedItem as ListViewItem)?.Tag as GamepadAssistantEntry;

    public GamepadAssistantWindow(Func<GamepadAction, Task> dispatch, Func<Task>? close = null)
    {
        this.dispatch = dispatch;
        Title = "手柄助手 · IMao";
        var root = new Grid { Padding = new Thickness(22), RowSpacing = 15 };
        GamepadWindowChrome.ApplyTheme(root);
        heading.Foreground = choices.Foreground = GamepadWindowChrome.Brush("IMaoTextBrush", 0xE7F0F7);
        message.Foreground = hint.Foreground = GamepadWindowChrome.Brush("IMaoSecondaryTextBrush", 0x9DACBD);
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.Children.Add(GamepadWindowChrome.Header(this, heading,
            close ?? (() => DispatchAsync(GamepadAction.Back)), "关闭手柄助手"));
        Grid.SetRow(message, 1); root.Children.Add(message);
        Grid.SetRow(choices, 2); root.Children.Add(choices);
        var footer = new StackPanel { Spacing = 10 };
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
        buttons.Children.Add(back); buttons.Children.Add(route);
        footer.Children.Add(buttons); footer.Children.Add(hint);
        Grid.SetRow(footer, 3); root.Children.Add(footer);
        // Avoid a second navigation path if WinUI also receives virtual gamepad key events.
        root.PreviewKeyDown += (_, e) => { if ((int)e.Key is >= 195 and <= 218) e.Handled = true; };
        choices.ItemClick += async (_, e) =>
        {
            if (rendering || e.ClickedItem is not ListViewItem item || !item.IsEnabled) return;
            choices.SelectedItem = item;
            await DispatchAsync(GamepadAction.Accept);
        };
        route.Click += async (_, _) => await DispatchAsync(GamepadAction.OpenRouteMenu);
        back.Click += async (_, _) => await DispatchAsync(GamepadAction.Back);
        Content = root;
        AppWindow.Resize(new SizeInt32(500, 650));
        chrome = new GamepadWindowChrome(this);
        Closed += (_, _) => IsClosed = true;
    }

    private async Task DispatchAsync(GamepadAction action)
    {
        try { await dispatch(action); }
        catch (Exception e) { if (!IsClosed) SetMessage("操作未完成：" + e.Message); }
    }

    internal void ShowEntries(string title, string notice, IReadOnlyList<GamepadAssistantEntry> entries, bool menu)
    {
        if (IsClosed) return;
        var previousPoint = SelectedEntry?.Selection;
        heading.Text = title; message.Text = notice;
        route.Visibility = menu ? Visibility.Collapsed : Visibility.Visible;
        back.Content = menu ? "B · 返回列表" : "B · 退出助手";
        hint.Text = menu ? "方向键 / 左摇杆选择 · A 确认 · B 返回" : "方向键 / 左摇杆选择 · A 查看攻略 · B 退出 · Y 路线操作";
        rendering = true;
        try
        {
            choices.Items.Clear();
            foreach (var entry in entries)
            {
                choices.Items.Add(new ListViewItem { Tag = entry, IsEnabled = entry.Enabled,
                    HorizontalContentAlignment = HorizontalAlignment.Stretch,
                    Content = new TextBlock { Text = entry.Label, FontSize = 17, TextWrapping = TextWrapping.Wrap,
                        Margin = new Thickness(6, 10, 6, 10) } });
            }
            int selected = entries.ToList().FindIndex(e => e.Enabled && previousPoint is not null &&
                e.Selection is { } point && point.ProfileId == previousPoint.ProfileId &&
                point.StateId == previousPoint.StateId && point.PointId == previousPoint.PointId);
            if (selected < 0) selected = entries.ToList().FindIndex(e => e.Enabled);
            choices.SelectedIndex = selected;
        }
        finally { rendering = false; }
        if (choices.SelectedItem is { } item) choices.ScrollIntoView(item);
    }

    internal void MoveSelection(int direction)
    {
        if (IsClosed || choices.Items.Count == 0 || direction == 0) return;
        int start = choices.SelectedIndex;
        for (int index = start < 0 ? 0 : start + Math.Sign(direction); index >= 0 && index < choices.Items.Count; index += Math.Sign(direction))
        {
            if (choices.Items[index] is not ListViewItem { IsEnabled: true } item) continue;
            choices.SelectedIndex = index;
            choices.ScrollIntoView(item);
            return;
        }
    }

    internal void ScrollContent(int direction)
    {
        if (IsClosed || FindScrollViewer(choices) is not { } scroll) return;
        scroll.ChangeView(null, Math.Clamp(scroll.VerticalOffset + direction * 90, 0, scroll.ScrollableHeight), null, true);
    }

    private static ScrollViewer? FindScrollViewer(DependencyObject parent)
    {
        if (parent is ScrollViewer scroll) return scroll;
        for (int index = 0; index < VisualTreeHelper.GetChildrenCount(parent); index++)
            if (FindScrollViewer(VisualTreeHelper.GetChild(parent, index)) is { } found) return found;
        return null;
    }

    internal void SetMessage(string value) { if (!IsClosed) message.Text = value; }
    internal void SetBusy(bool value)
    {
        if (IsClosed) return;
        choices.IsEnabled = !value; route.IsEnabled = !value;
    }

    internal void PlaceNearGame(IntPtr game)
    {
        if (!GetWindowRect(game, out var bounds)) return;
        var work = DisplayArea.GetFromRect(new RectInt32(bounds.Left, bounds.Top,
            Math.Max(1, bounds.Right - bounds.Left), Math.Max(1, bounds.Bottom - bounds.Top)), DisplayAreaFallback.Nearest).WorkArea;
        AppWindow.MoveAndResize(CalculatePlacement(new RectInt32(bounds.Left, bounds.Top,
            bounds.Right - bounds.Left, bounds.Bottom - bounds.Top), work, GetDpiForWindow(game)));
    }

    internal static RectInt32 CalculatePlacement(RectInt32 game, RectInt32 work, uint dpi)
    {
        double scale = (dpi == 0 ? 96 : dpi) / 96.0;
        int width = Math.Min((int)Math.Round(520 * scale), Math.Max(1, work.Width));
        int height = Math.Min((int)Math.Round(650 * scale), Math.Max(1, work.Height));
        return new RectInt32(Math.Clamp(game.X + (int)Math.Round(36 * scale), work.X, work.X + Math.Max(0, work.Width - width)),
            Math.Clamp(game.Y + (int)Math.Round(42 * scale), work.Y, work.Y + Math.Max(0, work.Height - height)), width, height);
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeRect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetWindowRect(IntPtr window, out NativeRect rect);
    [DllImport("user32.dll")]
    private static extern uint GetDpiForWindow(IntPtr window);
}
