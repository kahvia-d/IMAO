using System.ComponentModel;
using System.Globalization;
using IMao_WinUI.Helpers;
using IMao_WinUI.Models;
using IMao_WinUI.StringItems;
using Microsoft.UI.Dispatching;

namespace IMao_WinUI.Services;

public sealed record FilterSelectionChange(long Version, IReadOnlyList<string> ItemIds, bool Enabled);

public sealed class FilterSelectionRow : INotifyPropertyChanged
{
    public MapFilterItem Item { get; }
    private readonly bool english;
    private bool enabled;
    public string Id => Item.Id;
    public string Name => english && Item.Name != Item.Id ? Item.Name : Item.OfficialName;
    public string? IconPath => Item.IconPath is { } path ? new Uri(path).AbsoluteUri : null;
    public string Detail => $"{Item.OfficialName}\n{Item.Category}\n{Item.Id}";
    public bool IsEnabled => enabled;
    public event PropertyChangedEventHandler? PropertyChanged;
    internal FilterSelectionRow(MapFilterItem item, bool value, bool english) { Item = item; enabled = value; this.english = english; }
    internal void SetEnabled(bool value)
    {
        if (enabled == value) return;
        enabled = value;
        PropertyChanged?.Invoke(this, new(nameof(IsEnabled)));
    }
}

// One process-wide desired state. Local commits are authoritative; IPC acknowledgements
// only describe synchronization and never write selection state back over newer changes.
public sealed class FilterSelectionService : IDisposable
{
    private readonly object gate = new();
    private readonly DispatcherQueue? dispatcher = DispatcherQueue.GetForCurrentThread();
    private readonly Func<string[], bool, (bool Success, string Error)> persist;
    private readonly Func<IReadOnlyDictionary<string, bool>, CancellationToken, Task<bool>> synchronize;
    private readonly CancellationTokenSource lifetime = new();
    private readonly Dictionary<string, FilterSelectionRow> byId;
    private readonly CoreHostService? core;
    private bool connected, disposed, running, retryRequested;
    private long version = 1, epoch, acknowledgedVersion, acknowledgedEpoch = -1;
    private string saveError = "";
    private string syncError = "";
    private Task synchronization = Task.CompletedTask;
    public MapFilterCatalog Catalog { get; }
    public IReadOnlyList<FilterSelectionRow> Rows { get; }
    public bool English { get; }
    public string CatalogWarning { get; }
    public event EventHandler? Changed;
    public event EventHandler<FilterSelectionChange>? SelectionChanged;
    public long Version { get { lock (gate) return version; } }
    public bool IsSyncPending { get { lock (gate) return acknowledgedVersion != version || acknowledgedEpoch != epoch; } }
    public bool IsSynchronizing { get { lock (gate) return running; } }
    public bool HasSaveError { get { lock (gate) return saveError.Length > 0; } }
    public string SyncState { get { lock (gate) return saveError.Length > 0 ? "saveFailed" : running ? "syncing" : IsSyncPending ? "pending" : "saved"; } }
    public string Message
    {
        get
        {
            lock (gate)
            {
                if (saveError.Length > 0) return saveError;
                if (running) return English ? "Saved locally. Synchronizing…" : "已保存，正在同步…";
                if (IsSyncPending) return (English ? "Saved locally. Waiting to sync." : "已保存，等待同步。") +
                    (syncError.Length > 0 ? " " + syncError : "");
                return English ? "Saved and synchronized." : "筛选已保存并同步。";
            }
        }
    }

    public FilterSelectionService(CoreHostService coreHost)
    {
        core = coreHost;
        English = CultureInfo.CurrentUICulture.Name.StartsWith("en", StringComparison.OrdinalIgnoreCase);
        var strings = new StringItem();
        strings.LoadString(English ? "en-US" : "zh-CN");
        Catalog = MapFilterCatalog.Load(strings.itemsDatas.SelectMany(category => category.ItemDatas
            .Select(item => new MapFilterSourceItem(item.Id, item.Name_SpecifiedLanguage, category.Category))));
        var local = new LocalItemFilter();
        var values = local.GetFilteredItemsDatas().Where(item => item.Name is not null)
            .ToDictionary(item => item.Name!, item => item.Status == 1, StringComparer.Ordinal);
        CatalogWarning = string.Join("\n", new[] { Catalog.LastError, local.LastError }.Where(value => value.Length > 0));
        byId = Catalog.Items.ToDictionary(item => item.Id, item => new FilterSelectionRow(item, values.GetValueOrDefault(item.Id), English), StringComparer.Ordinal);
        Rows = Array.AsReadOnly(byId.Values.ToArray());
        persist = (ids, enabled) => (local.SetItemsStatus(ids, enabled ? 1 : 0), local.LastError);
        synchronize = core.SynchronizeFilterAsync;
        core.PropertyChanged += Core_PropertyChanged;
        SetConnected(core.IsConnected);
    }

    internal FilterSelectionService(MapFilterCatalog catalog, IReadOnlyDictionary<string, bool> initial,
        Func<string[], bool, (bool Success, string Error)> persist,
        Func<IReadOnlyDictionary<string, bool>, CancellationToken, Task<bool>> synchronize,
        bool english = false)
    {
        Catalog = catalog; CatalogWarning = catalog.LastError; English = english;
        this.persist = persist; this.synchronize = synchronize;
        byId = catalog.Items.ToDictionary(item => item.Id, item => new FilterSelectionRow(item, initial.GetValueOrDefault(item.Id), english), StringComparer.Ordinal);
        Rows = Array.AsReadOnly(byId.Values.ToArray());
    }

    public bool SetEnabled(IEnumerable<string> itemIds, bool enabled)
    {
        string[] changed;
        long committedVersion;
        lock (gate)
        {
            if (disposed) return false;
            changed = itemIds.Distinct(StringComparer.Ordinal).Where(id => byId.TryGetValue(id, out var row) && row.IsEnabled != enabled).ToArray();
            if (changed.Length == 0) return true;
            (bool Success, string Error) result;
            try { result = persist(changed, enabled); }
            catch (Exception error) { result = (false, error.Message); }
            if (!result.Success)
            {
                saveError = (English ? "Could not save filter settings: " : "无法保存筛选设置：") + result.Error;
                PublishChanged();
                return false;
            }
            saveError = ""; syncError = "";
            foreach (var id in changed) byId[id].SetEnabled(enabled);
            committedVersion = ++version;
            retryRequested = true;
        }
        PublishChanged();
        OnUi(() => SelectionChanged?.Invoke(this, new(committedVersion, Array.AsReadOnly(changed), enabled)));
        StartSynchronization();
        return true;
    }

    public void RetrySynchronization()
    {
        lock (gate) { if (disposed) return; retryRequested = true; }
        StartSynchronization();
    }

    private void Core_PropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(CoreHostService.IsConnected)) SetConnected(core!.IsConnected);
    }

    internal void SetConnected(bool value)
    {
        lock (gate)
        {
            if (disposed || connected == value) return;
            connected = value; epoch++;
            retryRequested = value;
        }
        PublishChanged();
        StartSynchronization();
    }

    private void StartSynchronization()
    {
        lock (gate)
        {
            if (disposed || running || !connected || !retryRequested) return;
            running = true;
            synchronization = SynchronizeLoopAsync();
        }
    }

    private async Task SynchronizeLoopAsync()
    {
        try
        {
            while (true)
            {
                Dictionary<string, bool> snapshot;
                long sentVersion, sentEpoch;
                lock (gate)
                {
                    if (disposed || !connected || !retryRequested) break;
                    retryRequested = false;
                    sentVersion = version; sentEpoch = epoch;
                    snapshot = byId.ToDictionary(pair => pair.Key, pair => pair.Value.IsEnabled, StringComparer.Ordinal);
                }
                PublishChanged();
                bool accepted;
                string failure = "";
                try { accepted = await synchronize(snapshot, lifetime.Token); }
                catch (OperationCanceledException) when (lifetime.IsCancellationRequested) { break; }
                catch (Exception error) { accepted = false; failure = error.Message; }
                lock (gate)
                {
                    if (disposed) break;
                    // A previous connection or previous local revision cannot acknowledge this state.
                    if (connected && epoch == sentEpoch && version == sentVersion)
                    {
                        if (accepted) { acknowledgedVersion = sentVersion; acknowledgedEpoch = sentEpoch; syncError = ""; }
                        else syncError = failure;
                    }
                    if (!connected) break;
                    if (version != sentVersion || epoch != sentEpoch) retryRequested = true;
                    if (!retryRequested) break;
                }
            }
        }
        finally
        {
            lock (gate) running = false;
            PublishChanged();
            StartSynchronization();
        }
    }

    internal Task WaitForSynchronizationAsync() { lock (gate) return synchronization; }
    private void PublishChanged() => OnUi(() => { if (!disposed) Changed?.Invoke(this, EventArgs.Empty); });
    private void OnUi(Action action)
    {
        if (dispatcher is null || dispatcher.HasThreadAccess) action();
        else dispatcher.TryEnqueue(() => action());
    }
    public void Dispose()
    {
        lock (gate) { if (disposed) return; disposed = true; }
        if (core is not null) core.PropertyChanged -= Core_PropertyChanged;
        lifetime.Cancel();
        // The in-flight sender may still be observing this token; do not dispose its source yet.
    }
}
