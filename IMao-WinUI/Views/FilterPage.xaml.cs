using System.ComponentModel;
using IMao_WinUI.Helpers;
using IMao_WinUI.Models;
using IMao_WinUI.Services;
using IMao_WinUI.StringItems;
using IMao_WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace IMao_WinUI.Views;

public sealed partial class FilterPage : Page
{
    private readonly LocalItemFilter localItemFilter = new();
    private readonly Dictionary<string, FilterRow> rows = new(StringComparer.Ordinal);
    private MapFilterCatalog catalog = null!;
    private List<FilterRow> visibleRows = new();
    private bool ready;
    private bool saving;
    private bool english;
    public FilterViewModel ViewModel { get; }

    public sealed record CategoryChoice(string Key, string Name, IReadOnlyList<MapFilterItem> Items);

    public sealed class FilterRow : INotifyPropertyChanged
    {
        public MapFilterItem Item { get; }
        private readonly bool english;
        public string Name => english && Item.Name != Item.Id ? Item.Name : Item.OfficialName;
        public string? IconPath => Item.IconPath is { } path ? new Uri(path).AbsoluteUri : null;
        public string Detail => $"{Item.OfficialName}\n{Item.Category}\n{Item.Id}";
        private bool enabled;
        public bool IsEnabled
        {
            get => enabled;
            set
            {
                if (enabled == value) return;
                enabled = value;
                PropertyChanged?.Invoke(this, new(nameof(IsEnabled)));
            }
        }
        public event PropertyChangedEventHandler? PropertyChanged;
        public FilterRow(MapFilterItem item, bool enabled, bool english) { Item = item; this.enabled = enabled; this.english = english; }
    }

    public FilterPage()
    {
        ViewModel = App.GetService<FilterViewModel>();
        InitializeComponent();
        english = language.Text.StartsWith("en", StringComparison.OrdinalIgnoreCase);
        var strings = new StringItem();
        strings.LoadString(language.Text);
        catalog = MapFilterCatalog.Load(strings.itemsDatas.SelectMany(category =>
            category.ItemDatas.Select(item => new MapFilterSourceItem(item.Id, item.Name_SpecifiedLanguage, category.Category))));
        var statuses = localItemFilter.GetFilteredItemsDatas()
            .Where(item => item.Name is not null).ToDictionary(item => item.Name!, item => item.Status == 1, StringComparer.Ordinal);
        foreach (var item in catalog.Items)
            rows[item.Id] = new FilterRow(item, statuses.GetValueOrDefault(item.Id), english);

        FilterDescription.Text = L("按库街区分类查找点位，也可按角色、武器选择养成材料。勾选后自动保存。",
            "Browse map categories or find materials by character and weapon. Changes are saved automatically.");
        SearchBox.PlaceholderText = L("搜索点位或材料名称", "Search points or materials");
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(SearchBox, SearchBox.PlaceholderText);
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(CategoryPicker, L("点位分类", "Map category"));
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(PresetPicker, L("角色或武器", "Character or weapon"));
        SelectedOnly.Content = L("仅看已选", "Selected only");
        SelectResults.Content = L("全选当前结果", "Select results");
        ClearResults.Content = L("取消当前结果", "Clear results");
        EmptyMessage.Text = L("没有符合条件的点位，请切换分类或修改搜索。", "No matching points. Try another category or search.");

        var choices = new List<CategoryChoice> { new("all", L("全部类型", "All categories"), catalog.Items) };
        choices.AddRange(catalog.Categories.Select(group => new CategoryChoice(group.Key, TranslateCategory(group.Name), group.Items)));
        if (catalog.CharacterGroups.Count > 0) choices.Add(new("characters", L("角色养成", "Character materials"), Array.Empty<MapFilterItem>()));
        if (catalog.WeaponGroups.Count > 0) choices.Add(new("weapons", L("武器养成", "Weapon materials"), Array.Empty<MapFilterItem>()));
        CategoryPicker.ItemsSource = choices;
        CategoryPicker.SelectedItem = choices.FirstOrDefault(choice => choice.Name == L("收集物", "Collectibles")) ?? choices[0];
        ready = true;
        RefreshResults();
        if (localItemFilter.LastError.Length > 0 || catalog.LastError.Length > 0)
        {
            FilterWarning.Message = string.Join("\n", new[] { localItemFilter.LastError, catalog.LastError }.Where(message => message.Length > 0));
            FilterWarning.IsOpen = true;
        }
    }

    private string L(string chinese, string translated) => english ? translated : chinese;
    private string TranslateCategory(string name) => !english ? name : name switch
    {
        "收集物" => "Collectibles", "探索" => "Exploration", "采集物" => "Gathering",
        "敌人" => "Enemies", "强敌" => "Elite enemies", "挑战" => "Challenges",
        "NPC及服务点" => "NPCs and services", "补充分类" => "Other points", _ => name
    };

    private void RefreshResults()
    {
        if (!ready) return;
        var choice = CategoryPicker.SelectedItem as CategoryChoice;
        IEnumerable<MapFilterItem> items = choice?.Items ?? catalog.Items;
        if (choice?.Key is "characters" or "weapons")
            items = (PresetPicker.SelectedItem as MapFilterGroup)?.Items ?? Array.Empty<MapFilterItem>();
        var query = SearchBox.Text.Trim();
        var group = PresetPicker.SelectedItem as MapFilterGroup;
        bool groupNameMatches = choice?.Key is "characters" or "weapons" && group is not null && query.Length > 0 &&
            (group.Name.Contains(query, StringComparison.OrdinalIgnoreCase) || group.Acronym.Contains(query, StringComparison.OrdinalIgnoreCase));
        visibleRows = items.Where(item => groupNameMatches || item.Matches(query)).Select(item => rows[item.Id])
            .Where(row => SelectedOnly.IsChecked != true || row.IsEnabled).ToList();
        FilterItems.ItemsSource = visibleRows;
        EmptyMessage.Visibility = visibleRows.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        UpdateSummary();
    }

    private void UpdateSummary()
    {
        var selected = rows.Values.Count(row => row.IsEnabled);
        SelectionSummary.Text = L($"已选 {selected} / {rows.Count} 类 · 当前 {visibleRows.Count} 类",
            $"{selected} / {rows.Count} selected · {visibleRows.Count} shown");
        SelectResults.IsEnabled = !saving && visibleRows.Any(row => !row.IsEnabled);
        ClearResults.IsEnabled = !saving && visibleRows.Any(row => row.IsEnabled);
    }

    private void CategoryPicker_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!ready) return;
        RefreshPresets(true);
        RefreshResults();
    }

    private void RefreshPresets(bool reset)
    {
        var key = (CategoryPicker.SelectedItem as CategoryChoice)?.Key;
        bool isPreset = key is "characters" or "weapons";
        var groups = key == "characters" ? catalog.CharacterGroups : catalog.WeaponGroups;
        var presets = isPreset ? groups.Where(group => group.Matches(SearchBox.Text)).ToArray() : Array.Empty<MapFilterGroup>();
        var previous = reset ? null : PresetPicker.SelectedItem as MapFilterGroup;
        ready = false;
        PresetPicker.Visibility = isPreset ? Visibility.Visible : Visibility.Collapsed;
        PresetPicker.ItemsSource = presets;
        PresetPicker.SelectedItem = previous is not null && presets.Contains(previous) ? previous : presets.FirstOrDefault();
        SearchBox.PlaceholderText = isPreset ? L("搜索角色、武器或材料", "Search characters, weapons or materials") : L("搜索点位或材料名称", "Search points or materials");
        ready = true;
    }
    private void PresetPicker_SelectionChanged(object sender, SelectionChangedEventArgs e) => RefreshResults();
    private void SearchBox_TextChanged(object sender, TextChangedEventArgs e)
    {
        if (!ready) return;
        RefreshPresets(false);
        RefreshResults();
    }
    private void SelectedOnly_Click(object sender, RoutedEventArgs e) => RefreshResults();
    private async void SelectResults_Click(object sender, RoutedEventArgs e) => await SaveSelectionAsync(visibleRows.ToArray(), true);
    private async void ClearResults_Click(object sender, RoutedEventArgs e) => await SaveSelectionAsync(visibleRows.ToArray(), false);

    private async void Item_Click(object sender, RoutedEventArgs e)
    {
        if (!ready || saving || sender is not CheckBox { Tag: FilterRow row } checkBox) return;
        var enabled = checkBox.IsChecked == true;
        if (row.IsEnabled == enabled) return;
        await SaveSelectionAsync(new[] { row }, enabled);
        if (ReferenceEquals(checkBox.Tag, row) && checkBox.IsChecked != row.IsEnabled)
            checkBox.IsChecked = row.IsEnabled;
    }

    private async Task SaveSelectionAsync(IEnumerable<FilterRow> targets, bool enabled)
    {
        if (saving) return;
        var changed = targets.Where(row => row.IsEnabled != enabled).ToArray();
        if (changed.Length == 0) return;
        var ids = changed.Select(row => row.Item.Id).ToArray();
        if (!localItemFilter.SetItemsStatus(ids, enabled ? 1 : 0))
        {
            FilterWarning.Message = localItemFilter.LastError;
            FilterWarning.IsOpen = true;
            App.GetService<CoreHostService>().ReportUserError(localItemFilter.LastError);
            return;
        }
        saving = true;
        IsEnabled = false;
        try
        {
            foreach (var row in changed) row.IsEnabled = enabled;
            await App.GetService<CoreHostService>().SetItemsEnabledAsync(ids, enabled);
        }
        finally
        {
            saving = false;
            IsEnabled = true;
            RefreshResults();
        }
    }
}
