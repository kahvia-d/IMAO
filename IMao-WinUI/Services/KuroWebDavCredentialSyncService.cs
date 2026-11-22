#nullable enable
using IMao_WinUI.Core.KuroSync;
using IMao_WinUI.Helpers;

namespace IMao_WinUI.Services;

/// <summary>一次 WebDAV 凭证同步的汇总结果。</summary>
public sealed record KuroWebDavSyncSummary(
    int Pulled,
    int Pushed,
    int Unchanged,
    IReadOnlyList<KuroWebDavAccountSummary> RemoteAccounts);

/// <summary>
/// 可选的 WebDAV 库街区凭证同步服务（默认关闭，作为旁路挂接，不影响现有本机凭证与库街区同步主流程）。
/// </summary>
public sealed class KuroWebDavCredentialSyncService
{
    private readonly KuroTokenVault vault;
    private readonly KuroWebDavSettingsStore settingsStore;
    private readonly Func<KuroWebDavSettings, KuroWebDavClient> clientFactory;
    private readonly SemaphoreSlim gate = new(1, 1);

    public KuroWebDavCredentialSyncService() : this(UserDataPaths.KuroSync, null) { }

    /// <summary>测试用构造函数：允许指定隔离目录与自定义 WebDAV 客户端工厂。</summary>
    internal KuroWebDavCredentialSyncService(string kuroSyncRoot, Func<KuroWebDavSettings, KuroWebDavClient>? clientFactory = null)
    {
        vault = new KuroTokenVault(kuroSyncRoot);
        settingsStore = new KuroWebDavSettingsStore(kuroSyncRoot);
        this.clientFactory = clientFactory ?? (s => new KuroWebDavClient(s.BaseUrl, s.Username, s.Password));
    }

    /// <summary>当前是否已启用且配置完整。</summary>
    public bool IsEnabled => settingsStore.Load().IsReady;

    /// <summary>读取当前保存的 WebDAV 配置。</summary>
    public KuroWebDavSettings LoadSettings() => settingsStore.Load();

    /// <summary>保存 WebDAV 配置（密码与同步口令自动通过 DPAPI 加密）。</summary>
    public void SaveSettings(KuroWebDavSettings settings) => settingsStore.Save(settings);

    /// <summary>测试指定 WebDAV 配置是否连通并返回云端已有账号列表。</summary>
    public async Task<IReadOnlyList<KuroWebDavAccountSummary>> TestConnectionAsync(
        KuroWebDavSettings settings, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(settings);
        if (string.IsNullOrWhiteSpace(settings.Passphrase))
            throw new ArgumentException("请输入同步加密口令。", nameof(settings));
        using var client = clientFactory(settings);
        return await client.TestConnectionAsync(cancellationToken);
    }

    /// <summary>获取云端当前已有凭证的账号列表。</summary>
    public async Task<IReadOnlyList<KuroWebDavAccountSummary>> ListRemoteAccountsAsync(CancellationToken cancellationToken = default)
    {
        var settings = settingsStore.Load();
        if (!settings.IsReady) return [];
        using var client = clientFactory(settings);
        return await client.ListAccountsAsync(cancellationToken);
    }

    /// <summary>
    /// 启动时在后台静默检查并拉取 WebDAV 上更新的凭证。
    /// 若未启用或网络暂时不可用，则静默跳过，绝不阻塞主程序启动。
    /// </summary>
    public async Task PullStartupAsync(CancellationToken cancellationToken = default)
    {
        try
        {
            if (!IsEnabled) return;
            await PullAllUpdatedAsync(cancellationToken);
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            // 启动拉取属于尽力而为，失败留给后续手动或掉线续命处理
        }
    }

    /// <summary>仅拉取 WebDAV 上比本机水位线更新的账号凭证（不主动推送）。</summary>
    public async Task<int> PullAllUpdatedAsync(CancellationToken cancellationToken = default)
    {
        var settings = settingsStore.Load();
        if (!settings.IsReady) return 0;
        await gate.WaitAsync(cancellationToken);
        try
        {
            using var client = clientFactory(settings);
            var remotes = await client.ListAccountsAsync(cancellationToken);
            var states = settingsStore.LoadSyncStates();
            int pulled = 0;
            foreach (var remote in remotes)
            {
                if (!KuroTokenVault.IsAccountId(remote.AccountId)) continue;
                bool hasLocal = vault.TryRead(KuroTokenVault.AccountCredentialId(remote.AccountId), out var local);
                long knownVersion = states.TryGetValue(remote.AccountId, out var st) ? st.LastSyncedVersion : 0;
                if (hasLocal && remote.Version <= knownVersion) continue;

                var downloaded = await client.DownloadAsync(remote.AccountId, settings.Passphrase, cancellationToken);
                if (downloaded is null) continue;
                var (envelope, payload) = downloaded.Value;
                if (!hasLocal || local.Token != payload.Token)
                {
                    vault.Save(KuroTokenVault.AccountCredentialId(remote.AccountId), payload.Token, remote.AccountId);
                    ++pulled;
                }
                settingsStore.RecordAccountSync(remote.AccountId, envelope.Version, payload.Token);
            }
            return pulled;
        }
        finally { gate.Release(); }
    }

    /// <summary>
    /// 针对单个库街区账号尝试从 WebDAV 刷新本机凭证（供“本机无凭证”或“库街区掉线自动续命”调用）。
    /// 当成功写入与原先不同的新 Token 时返回 true。
    /// </summary>
    public async Task<bool> TryRefreshAccountAsync(
        string accountId,
        string? rejectedToken = null,
        CancellationToken cancellationToken = default)
    {
        if (!KuroTokenVault.IsAccountId(accountId)) return false;
        var settings = settingsStore.Load();
        if (!settings.IsReady) return false;

        await gate.WaitAsync(cancellationToken);
        try
        {
            using var client = clientFactory(settings);
            var downloaded = await client.DownloadAsync(accountId, settings.Passphrase, cancellationToken);
            if (downloaded is null) return false;
            var (envelope, payload) = downloaded.Value;

            // 如果拉下来的跟刚刚被库街区拒绝的是同一个 Token，说明云端也还没更新，无需重试
            if (!string.IsNullOrEmpty(rejectedToken) && string.Equals(payload.Token, rejectedToken, StringComparison.Ordinal))
                return false;

            string credentialId = KuroTokenVault.AccountCredentialId(accountId);
            bool hasLocal = vault.TryRead(credentialId, out var local);
            if (hasLocal && string.Equals(local.Token, payload.Token, StringComparison.Ordinal))
            {
                settingsStore.RecordAccountSync(accountId, envelope.Version, payload.Token);
                return false;
            }

            vault.Save(credentialId, payload.Token, accountId);
            settingsStore.RecordAccountSync(accountId, envelope.Version, payload.Token);
            return true;
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            return false;
        }
        finally { gate.Release(); }
    }

    /// <summary>
    /// 双向同步本机 Vault 与 WebDAV：
    /// 1) 远端版本号大于本机水位线，或本机缺少该账号 → 从 WebDAV 拉取到本地；
    /// 2) 本机有该账号凭证、且 Token 相对上次同步发生变化（或远端不存在）→ 加密推送到 WebDAV。
    /// </summary>
    public async Task<KuroWebDavSyncSummary> SyncAllAsync(CancellationToken cancellationToken = default)
    {
        var settings = settingsStore.Load();
        if (!settings.IsReady)
            throw new InvalidOperationException("WebDAV 凭证同步尚未启用或配置不完整。");

        await gate.WaitAsync(cancellationToken);
        try
        {
            using var client = clientFactory(settings);
            await client.EnsureSyncDirectoryAsync(cancellationToken);
            var remotes = (await client.ListAccountsAsync(cancellationToken))
                .ToDictionary(item => item.AccountId, StringComparer.Ordinal);
            var states = settingsStore.LoadSyncStates();
            var localAccounts = vault.StoredAccounts().ToHashSet(StringComparer.Ordinal);
            var allAccountIds = remotes.Keys.Union(localAccounts, StringComparer.Ordinal)
                .OrderBy(id => id, StringComparer.Ordinal)
                .ToList();

            int pulled = 0, pushed = 0, unchanged = 0;
            foreach (string accountId in allAccountIds)
            {
                string credentialId = KuroTokenVault.AccountCredentialId(accountId);
                bool hasLocal = vault.TryRead(credentialId, out var local);
                bool hasRemote = remotes.TryGetValue(accountId, out var remote);
                states.TryGetValue(accountId, out var state);
                long knownVersion = state?.LastSyncedVersion ?? 0;
                bool localChanged = hasLocal && (state is null ||
                    !string.Equals(state.LastTokenHash, KuroWebDavSettingsStore.ComputeTokenHash(local.Token), StringComparison.Ordinal));

                if (hasRemote && (!hasLocal || remote!.Version > knownVersion))
                {
                    var downloaded = await client.DownloadAsync(accountId, settings.Passphrase, cancellationToken);
                    if (downloaded is not null)
                    {
                        var (envelope, payload) = downloaded.Value;
                        if (!hasLocal || !string.Equals(local.Token, payload.Token, StringComparison.Ordinal))
                        {
                            vault.Save(credentialId, payload.Token, accountId);
                            ++pulled;
                        }
                        else
                        {
                            ++unchanged;
                        }
                        settingsStore.RecordAccountSync(accountId, envelope.Version, payload.Token);
                        continue;
                    }
                }

                if (hasLocal && (!hasRemote || localChanged))
                {
                    var uploaded = await client.UploadAsync(accountId, local.Token, settings.Passphrase, "pc", "", "", cancellationToken);
                    settingsStore.RecordAccountSync(accountId, uploaded.Version, local.Token);
                    remotes[accountId] = new KuroWebDavAccountSummary(
                        uploaded.AccountId, uploaded.Version, uploaded.UpdatedAtUtc, uploaded.SourceDevice, uploaded.DisplayName);
                    ++pushed;
                    continue;
                }

                ++unchanged;
            }

            var updatedRemotes = remotes.Values
                .OrderBy(item => item.AccountId, StringComparer.Ordinal)
                .ToArray();
            return new KuroWebDavSyncSummary(pulled, pushed, unchanged, updatedRemotes);
        }
        finally { gate.Release(); }
    }

    /// <summary>主动删除 WebDAV 上指定账号的云端凭证。</summary>
    public async Task DeleteRemoteAccountAsync(string accountId, CancellationToken cancellationToken = default)
    {
        var settings = settingsStore.Load();
        if (!settings.IsReady)
            throw new InvalidOperationException("WebDAV 凭证同步尚未启用或配置不完整。");
        await gate.WaitAsync(cancellationToken);
        try
        {
            using var client = clientFactory(settings);
            await client.DeleteAsync(accountId, cancellationToken);
            settingsStore.RemoveAccountSync(accountId);
        }
        finally { gate.Release(); }
    }
}
