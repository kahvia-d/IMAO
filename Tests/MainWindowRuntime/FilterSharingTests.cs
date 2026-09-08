using IMao_WinUI.Models;
using IMao_WinUI.Services;
using IMao_WinUI.Views.Controls;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.Graphics;

namespace MainWindowRuntime;

internal static class FilterSharingTests
{
    public static async Task RunAsync(Window window, Action<bool, string> check)
    {
        string folder = Path.Combine(AppContext.BaseDirectory, "fixture", "shared-filter");
        Directory.CreateDirectory(Path.Combine(folder, "catalogs"));
        File.WriteAllText(Path.Combine(folder, "catalogs", "catalog-8.json"), """
            [
              {"id":"1","name":"角色","children":[{"id":"hero","name":"测试角色","acronym":"jueshe","children":[{"id":"a"},{"id":"b"}]}]},
              {"id":"2","name":"武器","children":[{"id":"weapon","name":"测试武器","children":[{"id":"b"},{"id":"c"}]}]},
              {"id":"3","name":"收集物","children":[{"id":"a","name":"声匣"},{"id":"b","name":"材料"},{"id":"c","name":"宝箱"}]}
            ]
            """);
        var catalog = MapFilterCatalog.Load(new[] { new MapFilterSourceItem("a", "声匣", "collect"), new("b", "材料", "collect"), new("c", "宝箱", "collect") }
            .Concat(Enumerable.Range(0, 400).Select(i => new MapFilterSourceItem("item" + i, "测试点 " + i, "extra"))), folder);
        var disk = catalog.Items.ToDictionary(item => item.Id, _ => false);
        bool failDisk = false;
        var requests = new List<(IReadOnlyDictionary<string, bool> State, TaskCompletionSource<bool> Ack)>();
        using var selection = new FilterSelectionService(catalog, disk,
            (ids, enabled) =>
            {
                if (failDisk) return (false, "fixture storage locked");
                foreach (var id in ids) disk[id] = enabled;
                return (true, "");
            },
            (values, token) =>
            {
                var ack = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
                requests.Add((new Dictionary<string, bool>(values), ack));
                return ack.Task.WaitAsync(token);
            });
        var commits = new List<FilterSelectionChange>();
        selection.SelectionChanged += (_, change) => commits.Add(change);
        selection.SetConnected(true);
        check(requests.Count == 1, "filter starts one synchronization for its current connection");
        selection.SetEnabled(["a"], true);
        selection.SetEnabled(["a"], false);
        check(requests.Count == 1 && !disk["a"], "rapid local edits commit without parallel IPC or waiting for an older acknowledgement");
        requests[0].Ack.SetResult(true);
        await Until(() => requests.Count == 2);
        check(!requests[1].State["a"] && selection.IsSyncPending, "old acknowledgement cannot clear newer desired state");
        requests[1].Ack.SetResult(false);
        await Until(() => !selection.IsSynchronizing);
        check(selection.IsSyncPending && selection.Message.Contains("等待同步"), "rejected core synchronization remains visibly pending");
        selection.SetConnected(false);
        selection.SetEnabled(["a"], true);
        check(requests.Count == 2 && disk["a"], "offline edits remain saved without starting the core");
        selection.SetConnected(true);
        await Until(() => requests.Count == 3);
        check(requests[2].State["a"] && !requests[2].State["b"], "reconnection sends latest complete controlled state");
        failDisk = true;
        long previousVersion = selection.Version;
        check(!selection.SetEnabled(["b"], true) && selection.Version == previousVersion && !selection.Rows.Single(row => row.Id == "b").IsEnabled &&
            !disk["b"], "failed local commit preserves both shared rows and prior disk state");
        requests[2].Ack.SetResult(true);
        await Until(() => !selection.IsSynchronizing);
        check(selection.HasSaveError && selection.Message.Contains("fixture storage locked"), "late IPC success does not overwrite a local save error");
        failDisk = false;
        selection.SetEnabled(["b"], true);
        await Until(() => requests.Count == 4);
        selection.SetConnected(false); selection.SetConnected(true);
        requests[3].Ack.SetResult(true);
        await Until(() => requests.Count == 5);
        check(selection.IsSyncPending && requests[4].State["a"] && requests[4].State["b"], "old connection acknowledgement cannot acknowledge a replacement connection");
        requests[4].Ack.SetResult(true);
        await Until(() => !selection.IsSynchronizing);
        check(!selection.IsSyncPending && commits.Count == 4, "only successful local changes emit the shared selection event");

        // Both visible controls use the same real service and row identities.
        selection.SetConnected(false);
        var left = new FilterControl(selection) { CompactMode = true };
        var right = new FilterControl(selection) { CompactMode = true };
        var panel = new Grid { Padding = new Thickness(16), ColumnSpacing = 16, RequestedTheme = ElementTheme.Dark };
        panel.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        panel.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        panel.Background = (Brush)Application.Current.Resources["IMaoCanvasBrush"];
        Grid.SetColumn(right, 1); panel.Children.Add(left); panel.Children.Add(right);
        window.Content = panel;
        window.AppWindow.Resize(new SizeInt32(1300, 650));
        await Task.Delay(180);
        SetCategory(left, "all"); SetCategory(right, "all");
        var leftGrid = (GridView)left.FindName("FilterItems");
        var rightGrid = (GridView)right.FindName("FilterItems");
        check(leftGrid.Items.Count == 403 && rightGrid.Items.Count == 403, "both filter surfaces retain the full item library");
        check(ReferenceEquals(leftGrid.Items[0], rightGrid.Items[0]), "both visible controls bind the same selection rows");
        left.FocusGamepad();
        check(left.HandleGamepad(GamepadAction.Accept) && ((ComboBox)left.FindName("CategoryPicker")).IsDropDownOpen, "gamepad A opens the actual category dropdown");
        left.HandleGamepad(GamepadAction.Down);
        check(left.HandleGamepad(GamepadAction.Back) && !((ComboBox)left.FindName("CategoryPicker")).IsDropDownOpen &&
            !left.HandleGamepad(GamepadAction.Back), "B closes an inner dropdown before returning control to the outer tool");
        SetCategory(left, "characters");
        check(leftGrid.Items.Count == 2 && ((ComboBox)left.FindName("PresetPicker")).Items.Count == 1, "character shortcut includes every referenced material");
        ((TextBox)left.FindName("SearchBox")).Text = "jueshe";
        await Task.Delay(80);
        check(leftGrid.Items.Count == 2, "character acronym search preserves the complete material shortcut");
        ((TextBox)left.FindName("SearchBox")).Text = "";
        SetCategory(left, "weapons");
        check(leftGrid.Items.Cast<FilterSelectionRow>().Select(row => row.Id).ToHashSet().SetEquals(["b", "c"]), "weapon shortcut shares exact existing IDs");
        SetCategory(left, "all"); left.FocusGamepad(); left.HandleGamepad(GamepadAction.Down);
        string firstId = ((FilterSelectionRow)leftGrid.Items[0]).Id;
        bool previous = selection.Rows.Single(row => row.Id == firstId).IsEnabled;
        left.HandleGamepad(GamepadAction.Accept);
        await Task.Delay(80);
        check(selection.Rows.Single(row => row.Id == firstId).IsEnabled != previous &&
            rightGrid.Items.Cast<FilterSelectionRow>().Single(row => row.Id == firstId).IsEnabled != previous,
            "gamepad item toggle updates the second visible control immediately");
        var secondContainer = rightGrid.ContainerFromIndex(0);
        check(secondContainer is not null && FindChild<CheckBox>(secondContainer)?.IsChecked == !previous, "second real CheckBox reflects the shared saved state");
        for (int i = 0; i < 12; i++) { left.HandleGamepad(GamepadAction.Down); await Task.Delay(30); }
        await Task.Delay(100);
        int beforeCommit = commits.Count;
        left.HandleGamepad(GamepadAction.Accept);
        check(commits.Count == beforeCommit + 1 && commits[^1].ItemIds.Single() != firstId, "gamepad reaches and toggles a previously unrealized item");
        int realized = Enumerable.Range(0, 403).Count(i => leftGrid.ContainerFromIndex(i) is not null);
        check(realized < 180, "shared control retains virtualization after gamepad scrolling (" + realized + "/403)");
        failDisk = true;
        var heldRow = selection.Rows.Single(row => row.Id == commits[^1].ItemIds.Single());
        bool heldValue = heldRow.IsEnabled;
        left.HandleGamepad(GamepadAction.Accept);
        check(heldRow.IsEnabled == heldValue && selection.HasSaveError, "gamepad failed save does not flip the shared checked value");
        await Until(() => FindChild<CheckBox>(leftGrid.ContainerFromItem(heldRow)) is { } box && ReferenceEquals(box.Tag, heldRow));
        var heldBox = FindChild<CheckBox>(leftGrid.ContainerFromItem(heldRow));
        check(heldBox is not null && ReferenceEquals(heldBox.Tag, heldRow), "currently focused virtualized checkbox represents the exact saved row");
        heldBox!.IsChecked = !heldValue;
        typeof(FilterControl).GetMethod("Item_Click", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance)!.Invoke(left, [heldBox, new RoutedEventArgs()]);
        check(heldBox.IsChecked == heldValue && heldRow.IsEnabled == heldValue, "failed pointer save restores the initiating real checkbox immediately");
        failDisk = false;
        SetCategory(left, "characters");
        var selectedOnly = (Microsoft.UI.Xaml.Controls.Primitives.ToggleButton)right.FindName("SelectedOnly");
        selectedOnly.IsChecked = true;
        typeof(FilterControl).GetMethod("SelectedOnly_Click", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance)!.Invoke(right, [selectedOnly, new RoutedEventArgs()]);
        selection.SetEnabled(["b"], false);
        check(!rightGrid.Items.Cast<FilterSelectionRow>().Any(row => row.Id == "b"), "external changes refresh the second control's selected-only view");
        // Removing one view releases its service subscription; reopening restores the latest snapshot.
        panel.Children.Remove(right);
        selection.SetEnabled(["c"], true);
        panel.Children.Add(right);
        await Task.Delay(80);
        check(rightGrid.Items.Cast<FilterSelectionRow>().Any(row => row.Id == "c"), "reloaded cached surface restores the latest saved selection");
        SetCategory(left, "characters");
        ((TextBox)left.FindName("SearchBox")).Text = "材料";
        string[] visible = leftGrid.Items.Cast<FilterSelectionRow>().Select(row => row.Id).ToArray();
        typeof(FilterControl).GetMethod("SelectResults_Click", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance)!.Invoke(left, [left.FindName("SelectResults"), new RoutedEventArgs()]);
        check(visible.Length == 1 && visible[0] == "b" && commits[^1].ItemIds.SequenceEqual(["b"]),
            "bulk action immediately commits only current material search results");
        check(rightGrid.Items.Cast<FilterSelectionRow>().Any(row => row.Id == "b") && ((TextBlock)right.FindName("SelectionSummary")).Text.Contains("已选 3 /"),
            "reloaded surface keeps observing subsequent commits; loaded=" + right.IsLoaded + "; count=" + rightGrid.Items.Count + "; summary=" + ((TextBlock)right.FindName("SelectionSummary")).Text);
        check(selection.Message.Contains("等待同步"), "both surfaces retain the explicit saved-waiting-for-sync state offline");
        ((TextBox)left.FindName("SearchBox")).Text = "";
        SetCategory(left, "all"); SetCategory(right, "all");
        await Task.Delay(650); panel.UpdateLayout();
        await App.Capture(panel, "shared-filter-controls.png");
        panel.Children.Remove(right); panel.ColumnDefinitions.RemoveAt(1);
        left.Height = 300; left.VerticalAlignment = VerticalAlignment.Top;
        window.AppWindow.Resize(new SizeInt32(1000, 520));
        await Task.Delay(180); panel.UpdateLayout();
        check(((GridView)left.FindName("FilterItems")).ActualHeight >= 88, "compact 300 DIP filter keeps a full virtualized card row");
        var bulkButton = (Button)left.FindName("SelectResults");
        var bulkBounds = bulkButton.TransformToVisual(left).TransformBounds(new Windows.Foundation.Rect(0, 0, bulkButton.ActualWidth, bulkButton.ActualHeight));
        check(bulkBounds.Bottom <= 300 && bulkBounds.Left >= 0, "compact filter bulk action remains visible");
        await App.Capture(panel, "800-filter-dock-compact.png");
        window.Content = null;
    }
    private static void SetCategory(FilterControl control, string key)
    {
        var combo = (ComboBox)control.FindName("CategoryPicker");
        combo.SelectedItem = combo.Items.Cast<FilterControl.CategoryChoice>().Single(choice => choice.Key == key);
    }
    private static async Task Until(Func<bool> condition)
    {
        for (int i = 0; i < 100 && !condition(); i++) await Task.Delay(10);
        if (!condition()) throw new TimeoutException("filter test condition timed out");
    }
    private static T? FindChild<T>(DependencyObject? root) where T : DependencyObject
    {
        if (root is null) return null;
        if (root is T target) return target;
        for (int i = 0; i < VisualTreeHelper.GetChildrenCount(root); i++)
            if (FindChild<T>(VisualTreeHelper.GetChild(root, i)) is { } found) return found;
        return null;
    }
}
