using IMao_WinUI.Contracts.Services;
using IMao_WinUI.Helpers;
using IMao_WinUI.Models;
using IMao_WinUI.Services;
using IMao_WinUI.ViewModels;
using Microsoft.UI.Xaml.Controls;
using System.Text.Json;

namespace IMao_WinUI.Views;

// 这一页只有两件事：讲清路线在游戏里怎么操作，以及把保存过的路线列出来（带类型图标）。
// 选点画布、生成预览、开始/暂停/跳过/完成都在游戏内大地图的「地图工具台 → 路径自动规划」里，
// 这里不再放那些入口：它们要么得到「请先打开大地图并完成识别」，要么静默无效。
//
// 合集与导入导出是这一页真正的工作面：它们**不需要游戏在跑**，而游戏内的列表需要一个能弹文件
// 对话框、能弹确认框的地方——那正是桌面。游戏内那边只多一条合集切换，好让「保存到当前合集」成立。
public sealed partial class FunctionPage : Page
{
    /// <summary>一条路线的文件名，用来给导出的路线包取一个默认名字。</summary>
    private const string RouteBundleFilter = "IMao 路线包 (*.json)\0*.json\0所有文件 (*.*)\0*.*\0\0";
    private readonly CoreHostService coreHost;
    private CancellationTokenSource? routePageLifetime;
    private RoutePlanningState renderedRouteState = new();
    private bool deletingRoute;
    // 列表当前按哪个合集显示："all" 是只读浏览，不改变"当前合集"，所以它不会把之后保存的路线
    // 带去别处。其余值就是合集 id，而点合集名同时也切换当前合集。
    private string browseCollection = "default";

    public FunctionViewModel ViewModel { get; }
    private bool BatchMode => RouteBatchBar.Visibility == Microsoft.UI.Xaml.Visibility.Visible;

    public FunctionPage()
    {
        ViewModel = App.GetService<FunctionViewModel>();
        coreHost = App.GetService<CoreHostService>();
        InitializeComponent();
        Loaded += FunctionPage_Loaded;
        Unloaded += FunctionPage_Unloaded;
    }

    private async void FunctionPage_Loaded(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        routePageLifetime?.Cancel();
        routePageLifetime?.Dispose();
        routePageLifetime = new();
        coreHost.RoutePlanningChanged -= CoreHost_RoutePlanningChanged;
        coreHost.RoutePlanningChanged += CoreHost_RoutePlanningChanged;
        RenderRouteState(coreHost.RoutePlanning);
        // The list is the one thing here the core does not push on its own: ask for it, so reopening the
        // page after changing routes in the game shows the current rows.
        try { await RouteCommandAsync("list"); }
        catch (Exception) { /* already reported on the message bar */ }
    }

    private void FunctionPage_Unloaded(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        coreHost.RoutePlanningChanged -= CoreHost_RoutePlanningChanged;
        routePageLifetime?.Cancel();
        routePageLifetime?.Dispose();
        routePageLifetime = null;
    }

    private void CoreHost_RoutePlanningChanged(object? sender, RoutePlanningState state) => RenderRouteState(state);

    private void RenderRouteState(RoutePlanningState state)
    {
        renderedRouteState = state;
        AutoRouteMessage.Severity = InfoBarSeverity.Informational;
        AutoRouteMessage.Message = state.Message;
        AutoRouteMessage.IsOpen = !string.IsNullOrWhiteSpace(state.Message);
        AutoRouteListSummary.Text = state.Active is { } activeRoute
            ? $"正在走的路线：{activeRoute.Name} · {state.NavigationLabel} · " +
              $"已完成 {activeRoute.Stops.Count(stop => stop.Completed)} / {activeRoute.Stops.Length} · " +
              $"当前目标 {(state.CurrentTarget?.DisplayName ?? "无")}。"
            : "当前没有在走的路线。开始指引在游戏内大地图的「路径自动规划」面板里做——这一页负责整理它们。";

        RenderCollections(state);

        // The dot on the row that is currently applied is set here rather than derived: a row cannot see
        // the state object that holds both the list and the current route. Records are `with`-copied all
        // over this codebase, so a flag on the previous snapshot's rows does not leak into this one.
        // `Active` is the one that matters here - this page lists saved routes and says which of them is
        // the route being followed - so it does not depend on the core also filling `CurrentRoute`.
        string? selectedId = (AutoRouteSavedRoutes.SelectedItem as SavedAutomaticRoute)?.Id;
        var currentId = state.Active?.Id ?? state.CurrentRoute?.Id ?? "";
        var rows = state.SavedRoutes
            .Where(saved => browseCollection == AllCollections || saved.Collection == browseCollection)
            .Select(saved => saved with { Current = currentId.Length > 0 && saved.Id == currentId, Batch = BatchMode })
            .ToArray();
        AutoRouteSavedRoutes.ItemsSource = rows;
        AutoRouteSavedRoutes.SelectedItem = rows.FirstOrDefault(row => row.Id == selectedId);
        AutoRouteEmptyHint.Visibility = rows.Length == 0 ? Microsoft.UI.Xaml.Visibility.Visible : Microsoft.UI.Xaml.Visibility.Collapsed;
        UpdateBatchSummary();
    }

    private const string AllCollections = "all";

    /// <summary>
    /// The collection bar. Every chip is a button because choosing a collection is the same act as
    /// entering it: the routes saved afterwards land there, in the game as well as here.
    /// </summary>
    private void RenderCollections(RoutePlanningState state)
    {
        RouteCollectionChips.Children.Clear();
        foreach (var collection in state.Collections)
        {
            var chip = new Button
            {
                Content = collection.Name + (collection.RouteCount > 0 ? $"（{collection.RouteCount}）" : ""),
                Tag = collection.Id,
                MinHeight = 34,
                Padding = new Microsoft.UI.Xaml.Thickness(12, 4, 12, 4),
                CornerRadius = new Microsoft.UI.Xaml.CornerRadius(8)
            };
            if (collection.Id == browseCollection) chip.Style = (Microsoft.UI.Xaml.Style)Microsoft.UI.Xaml.Application.Current.Resources["IMaoPrimaryButtonStyle"];
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(chip,
                $"{collection.Name}，{collection.RouteCount} 条路线{(collection.Current ? "，当前合集" : "")}");
            chip.Click += CollectionChip_Click;
            RouteCollectionChips.Children.Add(chip);
        }
        var all = new Button
        {
            Content = $"全部（{state.SavedRoutes.Length}）", Tag = AllCollections, MinHeight = 34,
            Padding = new Microsoft.UI.Xaml.Thickness(12, 4, 12, 4), CornerRadius = new Microsoft.UI.Xaml.CornerRadius(8)
        };
        if (browseCollection == AllCollections) all.Style = (Microsoft.UI.Xaml.Style)Microsoft.UI.Xaml.Application.Current.Resources["IMaoPrimaryButtonStyle"];
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(all, "显示全部合集里的路线，不改变当前合集");
        all.Click += CollectionChip_Click;
        RouteCollectionChips.Children.Add(all);

        var browsed = state.Collections.FirstOrDefault(collection => collection.Id == browseCollection);
        RouteCollectionRename.IsEnabled = browsed is { Editable: true };
        RouteCollectionDelete.IsEnabled = browsed is { Editable: true };
    }

    /// <summary>The collection the bar is showing, or null while every collection is being browsed.</summary>
    private RouteCollection? BrowsedCollection =>
        renderedRouteState.Collections.FirstOrDefault(collection => collection.Id == browseCollection);

    private async void CollectionChip_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (sender is not Button { Tag: string id } || id == browseCollection) return;
        browseCollection = id;
        RenderRouteState(renderedRouteState);
        // "全部" is a way of looking at the list, not a place to save into, so it sends nothing.
        if (id != AllCollections) await RouteCommandAsync("collectionCurrent", new { collectionId = id });
    }

    private async void RouteCollectionNew_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        var name = await AskNameAsync("新建合集", "合集名字", "", "新建");
        if (name is null) return;
        await RouteCommandAsync("collectionNew", new { name });
        // The core switches into a collection it just created; follow it so the list shows where the
        // next route will land.
        if (renderedRouteState.CurrentCollection.Length > 0) browseCollection = renderedRouteState.CurrentCollection;
        RenderRouteState(renderedRouteState);
    }

    private async void RouteCollectionRename_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (BrowsedCollection is not { Editable: true } browsed) return;
        var name = await AskNameAsync($"重命名「{browsed.Name}」", "合集名字", browsed.Name, "重命名");
        if (name is null || name == browsed.Name) return;
        await RouteCommandAsync("collectionRename", new { collectionId = browsed.Id, name });
    }

    private async void RouteCollectionDelete_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (BrowsedCollection is not { Editable: true } browsed) return;
        // Deleting a collection takes its routes with it. That is the rule the player chose, so the
        // confirmation has to say exactly how much it is about to remove.
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = $"删除合集「{browsed.Name}」？",
            Content = browsed.RouteCount == 0
                ? "这个合集是空的，删除它不会影响任何路线。"
                : $"这个合集里的 {browsed.RouteCount} 条路线会一起删除，无法撤销。点位的完成记录仍然保留。",
            PrimaryButtonText = browsed.RouteCount == 0 ? "删除合集" : $"删除合集和 {browsed.RouteCount} 条路线",
            CloseButtonText = "取消",
            DefaultButton = ContentDialogButton.Close
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary) return;
        browseCollection = "default";
        await RouteCommandAsync("collectionDelete", new { collectionId = browsed.Id });
        RenderRouteState(renderedRouteState);
    }

    /// <summary>
    /// A one-field question. The codebase has no dialog service, so this is the same inline
    /// ContentDialog the rest of the shell builds, with a TextBox in it.
    /// </summary>
    private async Task<string?> AskNameAsync(string title, string placeholder, string initial, string accept)
    {
        var box = new TextBox { Text = initial, PlaceholderText = placeholder, MaxLength = 40, SelectionStart = initial.Length };
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot, Title = title, Content = box,
            PrimaryButtonText = accept, CloseButtonText = "取消", DefaultButton = ContentDialogButton.Primary
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary) return null;
        var name = box.Text.Trim();
        return name.Length == 0 ? null : name;
    }

    private void RouteBatchEnter_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        RouteBatchBar.Visibility = Microsoft.UI.Xaml.Visibility.Visible;
        // The list's own selection and the tick boxes would mean two different "these ones" at once,
        // so the single-row delete stands down while the batch bar is up.
        AutoRouteDelete.IsEnabled = false;
        RenderRouteState(renderedRouteState);
    }

    private void RouteBatchExit_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        RouteBatchBar.Visibility = Microsoft.UI.Xaml.Visibility.Collapsed;
        AutoRouteDelete.IsEnabled = true;
        foreach (var row in Rows()) row.Selected = false;
        RenderRouteState(renderedRouteState);
    }

    private void RouteBatchSelectAll_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        // A second press clears: the button says 全选, and the state it is in says 清空.
        var rows = Rows();
        var select = rows.Any(row => !row.Selected);
        foreach (var row in rows) row.Selected = select;
        RenderRouteState(renderedRouteState);
    }

    private void RouteRowCheck_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e) =>
        // The flag itself is written through the two-way binding; this only refreshes the count.
        UpdateBatchSummary();

    private List<SavedAutomaticRoute> Rows() =>
        AutoRouteSavedRoutes.ItemsSource is IEnumerable<SavedAutomaticRoute> rows ? rows.ToList() : [];

    private List<SavedAutomaticRoute> CheckedRows() => Rows().Where(row => row.Selected).ToList();

    private void UpdateBatchSummary() =>
        RouteBatchSummary.Text = CheckedRows() is { Count: > 0 } checkedRows
            ? $"已选 {checkedRows.Count} 条：" + string.Join("、", checkedRows.Take(4).Select(row => row.DisplayLabel)) +
              (checkedRows.Count > 4 ? " …" : "")
            : "勾选下面的路线，然后导出、移动到别的合集，或者删掉它们。";

    private async void RouteBatchExport_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        var picked = CheckedRows();
        if (picked.Count == 0) { Report("请先勾选要导出的路线。", InfoBarSeverity.Informational); return; }
        var path = PickSavePath($"路线-{DateTime.Now:yyyyMMdd}.json");
        if (path is null) return;
        try
        {
            await RouteCommandAsync("export", new
            {
                path,
                routeIds = picked.Where(row => !row.Corrupt).Select(row => row.Id).ToArray()
            });
        }
        catch (Exception) { /* already reported on the message bar */ return; }
        ExitBatch();
    }

    private async void RouteExportCollection_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (browseCollection == AllCollections)
        {
            Report("「全部」只是把列表铺开，先点一个合集再导出它。", InfoBarSeverity.Informational);
            return;
        }
        if (renderedRouteState.Collections.FirstOrDefault(collection => collection.Id == browseCollection) is not { } collection) return;
        if (collection.RouteCount == 0) { Report($"合集「{collection.Name}」里还没有路线。", InfoBarSeverity.Informational); return; }
        var path = PickSavePath($"{collection.Name}-{DateTime.Now:yyyyMMdd}.json");
        if (path is null) return;
        try { await RouteCommandAsync("export", new { path, collectionId = collection.Id }); }
        catch (Exception) { /* already reported on the message bar */ }
    }

    private async void RouteBatchMove_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        var picked = CheckedRows();
        if (picked.Count == 0) { Report("请先勾选要移动的路线。", InfoBarSeverity.Informational); return; }
        var targets = renderedRouteState.Collections
            .Where(collection => picked.Any(row => row.Collection != collection.Id))
            .ToArray();
        if (targets.Length == 0) { Report("没有别的合集可以移动过去。", InfoBarSeverity.Informational); return; }
        var list = new ListView { SelectionMode = ListViewSelectionMode.Single, ItemsSource = targets.Select(collection => $"{collection.Name}（{collection.RouteCount}）").ToArray(), SelectedIndex = 0 };
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = $"把 {picked.Count} 条路线移动到…",
            Content = list, PrimaryButtonText = "移动", CloseButtonText = "取消", DefaultButton = ContentDialogButton.Primary
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary || list.SelectedIndex < 0) return;
        try
        {
            await RouteCommandAsync("routeCollection", new
            {
                routeIds = picked.Select(row => row.Id).ToArray(),
                collectionId = targets[list.SelectedIndex].Id
            });
        }
        catch (Exception) { /* already reported on the message bar */ return; }
        ExitBatch();
    }

    private async void RouteBatchDelete_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        var picked = CheckedRows();
        if (picked.Count == 0) { Report("请先勾选要删除的路线。", InfoBarSeverity.Informational); return; }
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = $"删除选中的 {picked.Count} 条路线？",
            Content = "只删除路线文件，点位的完成记录仍然保留。如果其中一条正在导航，导航会退出。",
            PrimaryButtonText = "删除", CloseButtonText = "取消", DefaultButton = ContentDialogButton.Close
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary) return;
        // The core removes one route per command and each is independently committed, so a failure
        // stops the run and says how far it got rather than pretending the rest went through.
        var done = 0;
        foreach (var row in picked)
        {
            try { await RouteCommandAsync("delete", new { routeId = row.Id, profileId = renderedRouteState.ProfileId }); done++; }
            catch (Exception exception)
            {
                Report($"已删除 {done} 条，第 {done + 1} 条失败：{exception.Message}", InfoBarSeverity.Error);
                ExitBatch();
                return;
            }
        }
        Report($"已删除 {done} 条路线，点位的完成记录保留。", InfoBarSeverity.Informational);
        ExitBatch();
    }

    private void ExitBatch()
    {
        RouteBatchBar.Visibility = Microsoft.UI.Xaml.Visibility.Collapsed;
        RenderRouteState(renderedRouteState);
    }

    /// <summary>
    /// Import is two steps on purpose: the core reads the package and answers what it holds, and only
    /// then is the player asked anything. The file's own kind decides the question, so the player
    /// never has to classify a file the file already describes.
    /// </summary>
    private async void RouteImport_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        var path = PickOpenPath("路线包.json");
        if (path is null) return;
        try { await RouteCommandAsync("importInspect", new { path }); }
        catch (Exception exception) { Report("无法读取这个路线包：" + exception.Message, InfoBarSeverity.Error); return; }
        if (renderedRouteState.Transfer is not { } transfer) return;
        var dialog = TransferDialog(transfer);
        if (dialog is null) return;
        var mode = ImportModeFor(transfer, await dialog.ShowAsync());
        if (mode is null) return;
        try { await RouteCommandAsync("importApply", new { path = transfer.Path, mode }); }
        catch (Exception exception) { Report("导入失败：" + exception.Message, InfoBarSeverity.Error); }
    }

    /// <summary>
    /// The question a package asks, built but not shown. Splitting it out is what lets the runtime
    /// harness assert the three branches — including the overwrite-or-new choice, which is the only
    /// decision a route package cannot answer for the player — without a modal window to dismiss.
    /// </summary>
    internal ContentDialog? TransferDialog(RouteBundleTransfer transfer)
    {
        if (!transfer.IsCollection)
        {
            var target = renderedRouteState.Collections.FirstOrDefault(collection => collection.Current);
            return new ContentDialog
            {
                XamlRoot = XamlRoot,
                Title = $"导入 {transfer.ImportableCount} 条路线？",
                Content = $"这批路线会加入当前合集「{target?.Name ?? "默认合集"}」{transfer.SkippedLabel}。" +
                    "如果某条路线的编号或名字已经被占用，导入的那条会换一个，不会顶掉已有的。",
                PrimaryButtonText = "导入", CloseButtonText = "取消", DefaultButton = ContentDialogButton.Primary
            };
        }
        if (transfer.Conflict is not { } clash)
            return new ContentDialog
            {
                XamlRoot = XamlRoot,
                Title = $"导入合集「{transfer.CollectionName}」？",
                Content = $"会新建这个合集，把 {transfer.ImportableCount} 条路线放进去，并切换过去{transfer.SkippedLabel}。",
                PrimaryButtonText = "导入", CloseButtonText = "取消", DefaultButton = ContentDialogButton.Primary
            };
        // The same name on both sides is the one case the file cannot answer for the player, so this
        // is the only place three buttons are offered.
        return new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = $"已经有合集「{clash.Name}」了",
            Content = $"「覆盖」会用导入的 {transfer.ImportableCount} 条路线换掉这个合集里现有的 {clash.RouteCount} 条" +
                $"（无法撤销）；「新建」会另外建一个不同名的合集，两边都留着。{transfer.SkippedLabel}",
            PrimaryButtonText = "覆盖", SecondaryButtonText = "新建", CloseButtonText = "取消",
            DefaultButton = ContentDialogButton.Secondary
        };
    }

    /// <summary>Which import the player just agreed to, or null when they closed the question.</summary>
    internal static string? ImportModeFor(RouteBundleTransfer transfer, ContentDialogResult choice) => choice switch
    {
        // A package of routes has nowhere else to go, so agreeing always means "into the current
        // collection". Only a collection package has a name to collide with.
        ContentDialogResult.Primary when !transfer.IsCollection => "routes",
        ContentDialogResult.Primary => transfer.Conflict is null ? "collectionNew" : "collectionOverwrite",
        ContentDialogResult.Secondary when transfer.IsCollection && transfer.Conflict is not null => "collectionNew",
        _ => null
    };

    private string? PickOpenPath(string suggested) =>
        OpenBundlePath is { } pick ? pick(suggested)
        : NativeFileDialog.Open(WinRT.Interop.WindowNative.GetWindowHandle(App.MainWindow), RouteBundleFilter, "选择 IMao 路线包");

    private string? PickSavePath(string suggested) =>
        SaveBundlePath is { } pick ? pick(suggested)
        : NativeFileDialog.Save(WinRT.Interop.WindowNative.GetWindowHandle(App.MainWindow), RouteBundleFilter, "导出路线包", suggested);

    /// <summary>
    /// Where the two file dialogs come from. They are the only thing on this page that cannot be
    /// exercised without a real desktop window sitting in front of it, so they are a seam rather than
    /// a direct call: the runtime harness substitutes one, and in production the answer is still the
    /// native common dialog. This is the same "hand the page's own thing in as a delegate" boundary
    /// the update flow already uses for its confirmation dialog.
    /// </summary>
    internal static Func<string, string?>? OpenBundlePath { get; set; }
    internal static Func<string, string?>? SaveBundlePath { get; set; }

    private void Report(string message, InfoBarSeverity severity)
    {
        AutoRouteMessage.Severity = severity;
        AutoRouteMessage.Message = message;
        AutoRouteMessage.IsOpen = true;
    }

    private void AutoRouteSavedRoutes_SelectionChanged(object sender, SelectionChangedEventArgs e) { }

    private async Task RouteCommandAsync(string action, object? arguments = null)
    {
        var lifetime = routePageLifetime;
        if (lifetime is null || lifetime.IsCancellationRequested) return;
        AutoRouteActions.IsEnabled = false;
        try
        {
            // Service applies only session-fenced snapshots; don't render this return value again.
            var payload = arguments is null ? new Dictionary<string, object?>() :
                JsonSerializer.Deserialize<Dictionary<string, object?>>(JsonSerializer.Serialize(arguments)) ?? new();
            if (action is not ("state" or "list") && !string.IsNullOrWhiteSpace(renderedRouteState.ProfileId))
                payload.TryAdd("profileId", renderedRouteState.ProfileId);
            await coreHost.ExecuteRoutePlanningAsync(action, payload, lifetime.Token);
        }
        catch (OperationCanceledException) when (lifetime.IsCancellationRequested) { }
        catch (Exception exception)
        {
            if (!ReferenceEquals(routePageLifetime, lifetime)) return;
            AutoRouteMessage.Severity = InfoBarSeverity.Error;
            AutoRouteMessage.Message = "自动路线操作失败：" + exception.Message;
            AutoRouteMessage.IsOpen = true;
            throw;
        }
        finally
        {
            if (ReferenceEquals(routePageLifetime, lifetime)) AutoRouteActions.IsEnabled = true;
        }
    }

    private async void AutoRouteAction_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (sender is Button { Tag: string action })
        {
            try { await RouteCommandAsync(action); }
            catch (Exception) { /* already reported on the bar */ }
        }
    }

    private async void AutoRouteDelete_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (deletingRoute) return;
        if (AutoRouteSavedRoutes.SelectedItem is not SavedAutomaticRoute selected)
        {
            Report("请先在路线列表中选择要删除的路线。", InfoBarSeverity.Informational);
            return;
        }
        var profile = renderedRouteState.ProfileId;
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = $"删除“{(string.IsNullOrWhiteSpace(selected.Name) ? selected.Id : selected.Name)}”？",
            Content = "将删除这条已保存的路线；如果它正在导航，也会退出导航。点位的完成记录仍然保留。",
            PrimaryButtonText = "删除路线",
            CloseButtonText = "取消",
            DefaultButton = ContentDialogButton.Close
        };
        deletingRoute = true;
        try
        {
            if (await dialog.ShowAsync() == ContentDialogResult.Primary)
            {
                try { await RouteCommandAsync("delete", new { routeId = selected.Id, profileId = profile }); }
                catch (Exception) { /* already reported on the bar */ }
            }
        }
        catch (Exception exception)
        {
            Report("无法删除路线：" + exception.Message, InfoBarSeverity.Error);
        }
        finally { deletingRoute = false; }
    }

    private void UsageGuide_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e) =>
        App.GetService<INavigationService>().NavigateTo(typeof(UsageGuideViewModel).FullName!);

}
