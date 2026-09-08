using IMao_WinUI.Models;
using IMao_WinUI.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace IMao_WinUI.Views.Controls;

public sealed partial class FilterControl : UserControl
{
    public sealed record CategoryChoice(string Key, string Name, IReadOnlyList<MapFilterItem> Items);
    private readonly FilterSelectionService selection;
    private readonly Dictionary<string, FilterSelectionRow> rows;
    private List<FilterSelectionRow> visibleRows = new();
    private bool ready, subscribed, compactMode, gamepadActive, gamepadInGrid;
    private int toolIndex, pointIndex, focusRevision;
    private string? focusedPointId;
    private Control? focusedTool;
    private Action? pendingPointFocus;
    private bool catalogNoticeOpen;
    public FilterSelectionService Selection => selection;
    public bool CompactMode
    {
        get => compactMode;
        set { compactMode = value; if (ready) UpdateCompactLayout(ActualHeight); }
    }
    public FilterControl() : this(App.GetService<FilterSelectionService>()) { }
    public FilterControl(FilterSelectionService service)
    {
        selection = service;
        rows = selection.Rows.ToDictionary(row => row.Id, StringComparer.Ordinal);
        InitializeComponent();
        FilterItems.LayoutUpdated += (_, _) => pendingPointFocus?.Invoke();
        CatalogNotice.Flyout.Opened += (_, _) => catalogNoticeOpen = true;
        CatalogNotice.Flyout.Closed += (_, _) => catalogNoticeOpen = false;
        FilterDescription.Text = L("按库街区分类查找点位，也可按角色、武器选择养成材料。勾选后自动保存。",
            "Browse categories or choose materials by character and weapon. Changes are saved automatically.");
        SearchBox.PlaceholderText = L("搜索点位或材料名称", "Search points or materials");
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(SearchBox, SearchBox.PlaceholderText);
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(CategoryPicker, L("点位分类", "Map category"));
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(PresetPicker, L("角色或武器", "Character or weapon"));
        SelectedOnly.Content = L("仅看已选", "Selected only");
        SelectResults.Content = L("全选当前结果", "Select results");
        ClearResults.Content = L("取消当前结果", "Clear results");
        EmptyMessage.Text = L("没有符合条件的点位，请切换分类或修改搜索。", "No matching points. Change the category or search.");
        var catalog = selection.Catalog;
        var choices = new List<CategoryChoice> { new("all", L("全部类型", "All categories"), catalog.Items) };
        choices.AddRange(catalog.Categories.Select(group => new CategoryChoice(group.Key, TranslateCategory(group.Name), group.Items)));
        if (catalog.CharacterGroups.Count > 0) choices.Add(new("characters", L("角色养成", "Character materials"), Array.Empty<MapFilterItem>()));
        if (catalog.WeaponGroups.Count > 0) choices.Add(new("weapons", L("武器养成", "Weapon materials"), Array.Empty<MapFilterItem>()));
        CategoryPicker.ItemsSource = choices;
        CategoryPicker.SelectedItem = choices.FirstOrDefault(choice => choice.Name == L("收集物", "Collectibles")) ?? choices[0];
        FilterWarning.Message = CatalogNoticeText.Text = selection.CatalogWarning;
        ready = true;
        RefreshResults();
        Loaded += Control_Loaded;
        Unloaded += Control_Unloaded;
    }
    private void Control_Loaded(object sender, RoutedEventArgs e) => ResumeSelection();
    private void ResumeSelection()
    {
        if (!subscribed) { selection.Changed += Selection_Changed; subscribed = true; }
        RefreshResults(); UpdateCompactLayout(ActualHeight);
    }
    private void Control_Unloaded(object sender, RoutedEventArgs e)
    {
        if (subscribed) { selection.Changed -= Selection_Changed; subscribed = false; }
        CategoryPicker.IsDropDownOpen = PresetPicker.IsDropDownOpen = false;
        CatalogNotice.Flyout.Hide();
        gamepadActive = false; pendingPointFocus = null; ++focusRevision;
        // WinUI may deliver an old Unloaded after the same control has been reparented
        // and Loaded again. Reconcile once after that event batch without retaining an
        // off-screen cached page's subscription.
        DispatcherQueue.TryEnqueue(() => { if (IsLoaded) ResumeSelection(); });
    }
    private void Selection_Changed(object? sender, EventArgs e) => RefreshResults();
    private string L(string chinese, string english) => selection.English ? english : chinese;
    private string TranslateCategory(string name) => !selection.English ? name : name switch
    {
        "收集物" => "Collectibles", "探索" => "Exploration", "采集物" => "Gathering", "敌人" => "Enemies",
        "强敌" => "Elite enemies", "挑战" => "Challenges", "NPC及服务点" => "NPCs and services", "补充分类" => "Other points", _ => name
    };
    private void FilterControl_SizeChanged(object sender, SizeChangedEventArgs e) => UpdateCompactLayout(e.NewSize.Height);
    private void UpdateCompactLayout(double height)
    {
        bool compact = CompactMode || height < 460;
        FilterEyebrow.Visibility = FilterDescription.Visibility = compact ? Visibility.Collapsed : Visibility.Visible;
        FilterTitle.Visibility = CompactMode ? Visibility.Collapsed : Visibility.Visible;
        FilterToolbar.Spacing = compact ? 6 : 10;
        FilterWarning.IsOpen = !compact && selection.CatalogWarning.Length > 0;
        CatalogNotice.Visibility = compact && selection.CatalogWarning.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        FilterToolbarScroll.MaxHeight = Math.Max(100, height - 108);
    }
    private void RefreshResults()
    {
        if (!ready) return;
        var choice = CategoryPicker.SelectedItem as CategoryChoice;
        IEnumerable<MapFilterItem> items = choice?.Items ?? selection.Catalog.Items;
        if (choice?.Key is "characters" or "weapons") items = (PresetPicker.SelectedItem as MapFilterGroup)?.Items ?? Array.Empty<MapFilterItem>();
        var query = SearchBox.Text.Trim();
        var group = PresetPicker.SelectedItem as MapFilterGroup;
        bool groupNameMatches = choice?.Key is "characters" or "weapons" && group is not null && query.Length > 0 &&
            (group.Name.Contains(query, StringComparison.OrdinalIgnoreCase) || group.Acronym.Contains(query, StringComparison.OrdinalIgnoreCase));
        var next = items.Where(item => groupNameMatches || item.Matches(query)).Select(item => rows[item.Id])
            .Where(row => SelectedOnly.IsChecked != true || row.IsEnabled).ToList();
        if (!next.SequenceEqual(visibleRows))
        {
            visibleRows = next; FilterItems.ItemsSource = visibleRows;
            if (gamepadInGrid && focusedPointId is not null)
            {
                int retained = visibleRows.FindIndex(row => row.Id == focusedPointId);
                // Removing the focused item never silently arms its successor for an A release.
                if (retained < 0) { gamepadInGrid = false; focusedPointId = null; toolIndex = 1; ++focusRevision; }
                else pointIndex = retained;
            }
        }
        EmptyMessage.Visibility = visibleRows.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        SelectionSummary.Text = L($"已选 {selection.Rows.Count(row => row.IsEnabled)} / {rows.Count} 类 · 当前 {visibleRows.Count} 类",
            $"{selection.Rows.Count(row => row.IsEnabled)} / {rows.Count} selected · {visibleRows.Count} shown");
        SelectResults.IsEnabled = visibleRows.Any(row => !row.IsEnabled);
        ClearResults.IsEnabled = visibleRows.Any(row => row.IsEnabled);
        SyncStatus.Text = selection.Message;
        SyncStatus.Foreground = (Brush)Application.Current.Resources[selection.HasSaveError ? "IMaoDangerBrush" : selection.IsSyncPending ? "IMaoWarningBrush" : "IMaoSecondaryTextBrush"];
        RetrySync.Visibility = selection.IsSyncPending && !selection.IsSynchronizing ? Visibility.Visible : Visibility.Collapsed;
        if (gamepadActive) UpdateGamepadHint();
    }
    private void RefreshPresets(bool reset)
    {
        var key = (CategoryPicker.SelectedItem as CategoryChoice)?.Key;
        bool isPreset = key is "characters" or "weapons";
        var groups = key == "characters" ? selection.Catalog.CharacterGroups : selection.Catalog.WeaponGroups;
        var presets = isPreset ? groups.Where(group => group.Matches(SearchBox.Text)).ToArray() : Array.Empty<MapFilterGroup>();
        var previous = reset ? null : PresetPicker.SelectedItem as MapFilterGroup;
        ready = false;
        PresetPicker.Visibility = isPreset ? Visibility.Visible : Visibility.Collapsed;
        PresetPicker.ItemsSource = presets;
        PresetPicker.SelectedItem = previous is not null && presets.Contains(previous) ? previous : presets.FirstOrDefault();
        SearchBox.PlaceholderText = isPreset ? L("搜索角色、武器或材料", "Search characters, weapons or materials") : L("搜索点位或材料名称", "Search points or materials");
        ready = true;
    }
    private void CategoryPicker_SelectionChanged(object sender, SelectionChangedEventArgs e) { if (ready) { RefreshPresets(true); RefreshResults(); } }
    private void PresetPicker_SelectionChanged(object sender, SelectionChangedEventArgs e) => RefreshResults();
    // TextChanged can be deferred while XAML is completing layout/focus changes. Apply
    // the current text synchronously so a following bulk action cannot use old results.
    private void SearchBox_TextChanging(TextBox sender, TextBoxTextChangingEventArgs e) { if (ready) { RefreshPresets(false); RefreshResults(); } }
    private void SelectedOnly_Click(object sender, RoutedEventArgs e) => RefreshResults();
    private void SelectResults_Click(object sender, RoutedEventArgs e) => selection.SetEnabled(visibleRows.Select(row => row.Id).ToArray(), true);
    private void ClearResults_Click(object sender, RoutedEventArgs e) => selection.SetEnabled(visibleRows.Select(row => row.Id).ToArray(), false);
    private void RetrySync_Click(object sender, RoutedEventArgs e) => selection.RetrySynchronization();
    private void Item_Click(object sender, RoutedEventArgs e)
    {
        if (!ready || sender is not CheckBox { Tag: FilterSelectionRow row } box) return;
        selection.SetEnabled([row.Id], box.IsChecked == true);
        // A failed disk commit must immediately restore the initiating CheckBox too.
        if (ReferenceEquals(box.Tag, row)) box.IsChecked = row.IsEnabled;
    }

    public void FocusGamepad()
    {
        if (!IsLoaded) return;
        gamepadActive = true; gamepadInGrid = false; focusedPointId = null;
        toolIndex = Math.Max(0, Tools().IndexOf(CategoryPicker));
        FocusTool(); UpdateGamepadHint();
    }
    public bool HandleGamepad(GamepadAction action)
    {
        if (!IsLoaded || !IsEnabled) return false;
        if (action == GamepadAction.Back)
        {
            if (catalogNoticeOpen) { CatalogNotice.Flyout.Hide(); return true; }
            if (CategoryPicker.IsDropDownOpen) { CategoryPicker.IsDropDownOpen = false; CategoryPicker.Focus(FocusState.Keyboard); return true; }
            if (PresetPicker.IsDropDownOpen) { PresetPicker.IsDropDownOpen = false; PresetPicker.Focus(FocusState.Keyboard); return true; }
            return false;
        }
        if (action is not (GamepadAction.Up or GamepadAction.Down or GamepadAction.Left or GamepadAction.Right or GamepadAction.Accept)) return false;
        if (!gamepadActive) FocusGamepad();
        var popup = CategoryPicker.IsDropDownOpen ? CategoryPicker : PresetPicker.IsDropDownOpen ? PresetPicker : null;
        if (popup is not null)
        {
            if (action == GamepadAction.Accept) { popup.IsDropDownOpen = false; popup.Focus(FocusState.Keyboard); }
            else if (popup.Items.Count > 0) popup.SelectedIndex = Math.Clamp(popup.SelectedIndex + (action is GamepadAction.Up or GamepadAction.Left ? -1 : 1), 0, popup.Items.Count - 1);
            UpdateGamepadHint(); return true;
        }
        if (gamepadInGrid)
        {
            if (visibleRows.Count == 0) { gamepadInGrid = false; FocusTool(); return true; }
            if (action == GamepadAction.Accept)
            {
                var target = visibleRows.FirstOrDefault(row => row.Id == focusedPointId);
                if (target is not null) selection.SetEnabled([target.Id], !target.IsEnabled);
            }
            else
            {
                int columns = Math.Max(1, (int)(FilterItems.ActualWidth / 232));
                int delta = action switch { GamepadAction.Left => -1, GamepadAction.Right => 1, GamepadAction.Up => -columns, _ => columns };
                if (pointIndex + delta < 0) { gamepadInGrid = false; focusedPointId = null; FocusTool(); }
                else { pointIndex = Math.Clamp(pointIndex + delta, 0, visibleRows.Count - 1); FocusPoint(); }
            }
        }
        else if (action == GamepadAction.Down && visibleRows.Count > 0)
        {
            gamepadInGrid = true; pointIndex = 0; FocusPoint();
        }
        else if (action == GamepadAction.Accept)
        {
            var tools = Tools(); if (tools.Count == 0) return true;
            // A disabled/disappearing action must not silently become its opposite action.
            if (focusedTool is not null && !tools.Contains(focusedTool)) { toolIndex = Math.Max(0, tools.IndexOf(CategoryPicker)); FocusTool(); return true; }
            if (focusedTool is not null) toolIndex = tools.IndexOf(focusedTool);
            toolIndex = Math.Clamp(toolIndex, 0, tools.Count - 1);
            var tool = tools[toolIndex];
            if (tool is ComboBox combo) combo.IsDropDownOpen = true;
            else if (ReferenceEquals(tool, SelectedOnly)) { SelectedOnly.IsChecked = SelectedOnly.IsChecked != true; RefreshResults(); }
            else if (ReferenceEquals(tool, SelectResults)) SelectResults_Click(tool, new());
            else if (ReferenceEquals(tool, ClearResults)) ClearResults_Click(tool, new());
            else if (ReferenceEquals(tool, RetrySync)) selection.RetrySynchronization();
            else if (ReferenceEquals(tool, CatalogNotice)) CatalogNotice.Flyout?.ShowAt(CatalogNotice);
            else tool.Focus(FocusState.Keyboard); // Search remains a normal TextBox with keyboard/IME input.
        }
        else
        {
            var tools = Tools();
            toolIndex = Math.Clamp(toolIndex + (action is GamepadAction.Left or GamepadAction.Up ? -1 : 1), 0, Math.Max(0, tools.Count - 1));
            FocusTool();
        }
        UpdateGamepadHint(); return true;
    }
    private List<Control> Tools() => new Control[] { SearchBox, CategoryPicker, PresetPicker, SelectedOnly, SelectResults, ClearResults, CatalogNotice, RetrySync }
        .Where(control => control.Visibility == Visibility.Visible && control.IsEnabled).ToList();
    private void FocusTool()
    {
        pendingPointFocus = null;
        var tools = Tools(); if (tools.Count == 0) return;
        toolIndex = Math.Clamp(toolIndex, 0, tools.Count - 1);
        focusedTool = tools[toolIndex];
        tools[toolIndex].StartBringIntoView(new() { AnimationDesired = false });
        tools[toolIndex].Focus(FocusState.Keyboard);
    }
    private void FocusPoint()
    {
        if (visibleRows.Count == 0) return;
        pointIndex = Math.Clamp(pointIndex, 0, visibleRows.Count - 1);
        var row = visibleRows[pointIndex]; focusedPointId = row.Id;
        int revision = ++focusRevision;
        FilterItems.ScrollIntoView(row);
        FilterItems.UpdateLayout();
        pendingPointFocus = FocusRealized;
        FocusRealized();
        DispatcherQueue.TryEnqueue(FocusRealized);
        void FocusRealized()
        {
            if (!IsLoaded || revision != focusRevision || !gamepadInGrid || focusedPointId != row.Id)
            {
                if (pendingPointFocus == FocusRealized) pendingPointFocus = null;
                return;
            }
            // ContainerFromItem can already map the new item while a recycled template
            // still contains the previous row. Wait for its actual bound row before focusing.
            if (FilterItems.ContainerFromItem(row) is DependencyObject container && FindPointCheckBox(container, row) is { } box)
            {
                pendingPointFocus = null;
                box.Focus(FocusState.Keyboard);
            }
        }
    }
    private static CheckBox? FindPointCheckBox(DependencyObject root, FilterSelectionRow row)
    {
        if (root is CheckBox box && ReferenceEquals(box.Tag, row)) return box;
        for (int i = 0; i < VisualTreeHelper.GetChildrenCount(root); i++)
            if (FindPointCheckBox(VisualTreeHelper.GetChild(root, i), row) is { } child) return child;
        return null;
    }
    private void UpdateGamepadHint()
    {
        GamepadHint.Visibility = Visibility.Visible;
        GamepadHint.Text = gamepadInGrid && focusedPointId is not null
            ? L($"点位 {pointIndex + 1} / {visibleRows.Count} · 方向键移动，A 勾选，↑ 返回工具，B 返回", $"Point {pointIndex + 1}/{visibleRows.Count} · Move, A toggle, Up to tools, B back")
            : L("←→ 切换工具 · ↓ 进入点位 · A 确认 · B 关闭下拉或返回；搜索支持键盘和输入法", "Left/right tools · Down points · A confirm · B close dropdown or back; search uses keyboard/IME");
    }
}
