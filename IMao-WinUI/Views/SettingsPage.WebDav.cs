#nullable enable
using IMao_WinUI.Core.KuroSync;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.ApplicationModel.DataTransfer;

namespace IMao_WinUI.Views;

/// <summary>
/// 设置页的 WebDAV 库街区凭证同步分部类（可选功能，默认关闭）。
/// </summary>
public sealed partial class SettingsPage
{
    private bool restoringWebDav = true;
    private bool webDavBusy;
    private bool webDavExpanded;

    private void RestoreWebDavSettings()
    {
        restoringWebDav = true;
        try
        {
            var current = kuroWebDav.LoadSettings();
            WebDavSyncEnabled.IsOn = current.Enabled;
            WebDavBaseUrlBox.Text = current.BaseUrl;
            WebDavUsernameBox.Text = current.Username;
            WebDavPasswordBox.Password = current.Password;
            WebDavPassphraseBox.Password = current.Passphrase;
            // 配置项默认折叠，保持页面简洁；仅当已开启但尚未填完必填项时自动展开方便填写
            webDavExpanded = current.Enabled && !current.IsReady;
            ApplyWebDavHeaderAndFoldState(current);
        }
        finally { restoringWebDav = false; }
    }

    private void ApplyWebDavHeaderAndFoldState(KuroWebDavSettings current)
    {
        bool enabled = WebDavSyncEnabled.IsOn;
        WebDavExpandButton.Visibility = enabled ? Visibility.Visible : Visibility.Collapsed;
        WebDavQuickSyncButton.Visibility = enabled && current.IsReady && !webDavExpanded ? Visibility.Visible : Visibility.Collapsed;
        WebDavExpandButton.Content = webDavExpanded ? "收起配置" : "展开配置";
        WebDavConfigPanel.Visibility = enabled && webDavExpanded ? Visibility.Visible : Visibility.Collapsed;

        if (!enabled)
        {
            WebDavSummaryText.Text = "仅通过您自己的 WebDAV 加密同步库街区登录凭证（大号/小号按账号 ID 隔离），点位进度仍走上方库街区同步。未开启时完全不发请求、不影响现有流程。";
        }
        else if (current.IsReady && Uri.TryCreate(current.BaseUrl, UriKind.Absolute, out var uri))
        {
            WebDavSummaryText.Text = $"已启用 WebDAV 凭证同步（{uri.Host} · 账号 {current.Username}）。启动或掉线时自动续命，平时可保持折叠。";
        }
        else
        {
            WebDavSummaryText.Text = "已开启开关，请展开并填写 WebDAV 地址、用户名、应用密码与同步加密口令后保存。";
        }
    }

    private void WebDavExpand_Click(object sender, RoutedEventArgs e)
    {
        webDavExpanded = !webDavExpanded;
        ApplyWebDavHeaderAndFoldState(ReadWebDavDraft());
    }

    private KuroWebDavSettings ReadWebDavDraft(bool? enabledOverride = null) => new(
        enabledOverride ?? WebDavSyncEnabled.IsOn,
        WebDavBaseUrlBox.Text.Trim(),
        WebDavUsernameBox.Text.Trim(),
        WebDavPasswordBox.Password,
        WebDavPassphraseBox.Password);

    private void ShowWebDavMessage(InfoBarSeverity severity, string message)
    {
        if (!webDavExpanded)
        {
            // 折叠状态下点击快捷同步时，用全局浮动轻提示反馈结果，不强行撑开卡片
            IMao_WinUI.Helpers.AppToast.Show(message, severity);
            return;
        }
        WebDavMessage.Severity = severity;
        WebDavMessage.Message = message;
        WebDavMessage.Visibility = Visibility.Visible;
        WebDavMessage.IsOpen = true;
    }

    private void SetWebDavBusy(bool busy)
    {
        webDavBusy = busy;
        WebDavSaveButton.IsEnabled = !busy;
        WebDavTestButton.IsEnabled = !busy;
        WebDavSyncNowButton.IsEnabled = !busy;
        WebDavQuickSyncButton.IsEnabled = !busy;
    }

    private void WebDavSyncEnabled_Toggled(object sender, RoutedEventArgs e)
    {
        if (restoringWebDav) return;
        bool enabled = WebDavSyncEnabled.IsOn;
        var draft = ReadWebDavDraft(enabled);

        if (enabled)
        {
            // 刚打开开关时若尚未配置完成则自动展开，已配置则保持简洁折叠
            webDavExpanded = !draft.IsReady;
            ApplyWebDavHeaderAndFoldState(draft);
            if (draft.IsReady)
            {
                try { kuroWebDav.SaveSettings(draft); }
                catch { }
            }
            return;
        }

        // 关闭开关时立即落盘停用状态并折叠，确保彻底静默不再发请求
        webDavExpanded = false;
        ApplyWebDavHeaderAndFoldState(draft);
        try
        {
            var saved = kuroWebDav.LoadSettings();
            kuroWebDav.SaveSettings(saved with { Enabled = false });
            WebDavAccountsCard.Visibility = Visibility.Collapsed;
            WebDavMessage.IsOpen = false;
            WebDavMessage.Visibility = Visibility.Collapsed;
        }
        catch (Exception ex)
        {
            ShowWebDavMessage(InfoBarSeverity.Warning, $"保存停用状态失败：{ex.Message}");
        }
    }

    private void WebDavSave_Click(object sender, RoutedEventArgs e)
    {
        var draft = ReadWebDavDraft();
        if (draft.Enabled && !draft.IsReady)
        {
            ShowWebDavMessage(InfoBarSeverity.Warning, "请完整填写 WebDAV 地址、用户名、应用密码和同步加密口令。");
            return;
        }
        try
        {
            kuroWebDav.SaveSettings(draft);
            ApplyWebDavHeaderAndFoldState(draft);
            ShowWebDavMessage(InfoBarSeverity.Success, "已安全保存 WebDAV 配置（密码与同步口令已使用本机 Windows DPAPI 加密）。");
        }
        catch (Exception ex)
        {
            ShowWebDavMessage(InfoBarSeverity.Error, $"保存配置失败：{ex.Message}");
        }
    }

    private async void WebDavTest_Click(object sender, RoutedEventArgs e)
    {
        if (webDavBusy) return;
        var draft = ReadWebDavDraft(enabledOverride: true);
        if (!draft.IsReady)
        {
            ShowWebDavMessage(InfoBarSeverity.Warning, "请先完整填写 WebDAV 地址、用户名、应用密码和同步加密口令。");
            return;
        }
        SetWebDavBusy(true);
        try
        {
            var accounts = await kuroWebDav.TestConnectionAsync(draft);
            RenderWebDavAccounts(accounts);
            ShowWebDavMessage(InfoBarSeverity.Success,
                accounts.Count == 0
                    ? "WebDAV 连接成功，同步目录已就绪（云端暂无账号凭证）。"
                    : $"WebDAV 连接成功，云端已有 {accounts.Count} 个库街区账号的加密凭证。");
        }
        catch (Exception ex)
        {
            ShowWebDavMessage(InfoBarSeverity.Error, $"连接测试失败：{ex.Message}");
        }
        finally { SetWebDavBusy(false); }
    }

    private async void WebDavSyncNow_Click(object sender, RoutedEventArgs e)
    {
        if (webDavBusy) return;
        var draft = ReadWebDavDraft();
        if (!draft.IsReady)
        {
            ShowWebDavMessage(InfoBarSeverity.Warning, "请先开启开关并完整填写 WebDAV 配置后再同步。");
            return;
        }
        SetWebDavBusy(true);
        try
        {
            // 同步前先保存当前表单修改，避免用户改完忘了点保存
            kuroWebDav.SaveSettings(draft);
            var summary = await kuroWebDav.SyncAllAsync();
            RenderWebDavAccounts(summary.RemoteAccounts);
            // 同步后刷新本机记录本凭据状态，让左侧/上方立即感知到新拉下来的账号
            RenderSyncLedger();
            ShowWebDavMessage(InfoBarSeverity.Success,
                $"凭证同步完成：从云端拉取 {summary.Pulled} 个，上传到云端 {summary.Pushed} 个，无变化 {summary.Unchanged} 个。");
        }
        catch (Exception ex)
        {
            ShowWebDavMessage(InfoBarSeverity.Error, $"凭证同步失败：{ex.Message}");
        }
        finally { SetWebDavBusy(false); }
    }

    private void WebDavCopyConfig_Click(object sender, RoutedEventArgs e)
    {
        var draft = ReadWebDavDraft(enabledOverride: true);
        if (!draft.IsReady)
        {
            ShowWebDavMessage(InfoBarSeverity.Warning, "请先填完四项配置，再复制给浏览器扩展使用。");
            return;
        }
        try
        {
            string encoded = KuroWebDavCrypto.EncodeConfigBundle(new KuroWebDavConfigBundle(
                draft.BaseUrl, draft.Username, draft.Password, draft.Passphrase));
            var package = new DataPackage();
            package.SetText(encoded);
            Clipboard.SetContent(package);
            ShowWebDavMessage(InfoBarSeverity.Informational,
                "已复制便携配置文本到剪贴板（含密码与口令，请仅粘贴给自己的浏览器扩展或设备，切勿外传）。");
        }
        catch (Exception ex)
        {
            ShowWebDavMessage(InfoBarSeverity.Error, $"复制配置文本失败：{ex.Message}");
        }
    }

    private async void WebDavPasteConfig_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            var view = Clipboard.GetContent();
            if (!view.Contains(StandardDataFormats.Text))
            {
                ShowWebDavMessage(InfoBarSeverity.Warning, "剪贴板里没有文本内容。");
                return;
            }
            string text = await view.GetTextAsync();
            if (!KuroWebDavCrypto.TryDecodeConfigBundle(text, out var bundle, out string error))
            {
                ShowWebDavMessage(InfoBarSeverity.Warning, error);
                return;
            }
            WebDavBaseUrlBox.Text = bundle.BaseUrl;
            WebDavUsernameBox.Text = bundle.Username;
            WebDavPasswordBox.Password = bundle.Password;
            WebDavPassphraseBox.Password = bundle.Passphrase;
            ShowWebDavMessage(InfoBarSeverity.Success, "已填入便携配置文本，请点击「保存配置」或「测试连接」。");
        }
        catch (Exception ex)
        {
            ShowWebDavMessage(InfoBarSeverity.Error, $"读取剪贴板失败：{ex.Message}");
        }
    }

    private void RenderWebDavAccounts(IReadOnlyList<KuroWebDavAccountSummary> accounts)
    {
        WebDavAccountsGrid.Children.Clear();
        WebDavAccountsGrid.RowDefinitions.Clear();
        WebDavAccountsGrid.ColumnDefinitions.Clear();
        if (accounts.Count == 0)
        {
            WebDavAccountsCard.Visibility = Visibility.Collapsed;
            return;
        }

        WebDavAccountsCard.Visibility = Visibility.Visible;
        WebDavAccountsGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        WebDavAccountsGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        WebDavAccountsGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        WebDavAccountsGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

        for (int row = 0; row < accounts.Count; ++row)
        {
            var item = accounts[row];
            WebDavAccountsGrid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

            string label = string.IsNullOrWhiteSpace(item.DisplayName)
                ? $"账号 {item.AccountId}"
                : $"{item.DisplayName}（{item.AccountId}）";
            var title = new TextBlock
            {
                Text = label,
                FontWeight = FontWeights.SemiBold,
                VerticalAlignment = VerticalAlignment.Center
            };
            Grid.SetRow(title, row);
            Grid.SetColumn(title, 0);
            WebDavAccountsGrid.Children.Add(title);

            var version = new TextBlock
            {
                Text = $"v{item.Version} · {FormatDeviceName(item.SourceDevice)}",
                VerticalAlignment = VerticalAlignment.Center,
                Style = (Style)Application.Current.Resources["IMaoSecondaryTextStyle"]
            };
            Grid.SetRow(version, row);
            Grid.SetColumn(version, 1);
            WebDavAccountsGrid.Children.Add(version);

            var updated = new TextBlock
            {
                Text = item.UpdatedAtUtc.ToLocalTime().ToString("MM-dd HH:mm"),
                VerticalAlignment = VerticalAlignment.Center,
                Style = (Style)Application.Current.Resources["IMaoSecondaryTextStyle"]
            };
            Grid.SetRow(updated, row);
            Grid.SetColumn(updated, 2);
            WebDavAccountsGrid.Children.Add(updated);

            var removeBtn = new Button
            {
                Content = "清除云端",
                Tag = item.AccountId,
                Style = (Style)Application.Current.Resources["IMaoSecondaryButtonStyle"],
                Padding = new Thickness(10, 4, 10, 4)
            };
            removeBtn.Click += WebDavDeleteRemoteAccount_Click;
            Grid.SetRow(removeBtn, row);
            Grid.SetColumn(removeBtn, 3);
            WebDavAccountsGrid.Children.Add(removeBtn);
        }
    }

    private async void WebDavDeleteRemoteAccount_Click(object sender, RoutedEventArgs e)
    {
        if (webDavBusy || sender is not Button { Tag: string accountId }) return;
        var dialog = new ContentDialog
        {
            Title = "清除 WebDAV 云端凭证",
            Content = $"确定要从 WebDAV 删除库街区账号 {accountId} 的云端加密凭证吗？不会删除本机已保存的凭证或点位记录。",
            PrimaryButtonText = "清除云端",
            CloseButtonText = "取消",
            DefaultButton = ContentDialogButton.Close,
            XamlRoot = XamlRoot
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary) return;

        SetWebDavBusy(true);
        try
        {
            await kuroWebDav.DeleteRemoteAccountAsync(accountId);
            var remaining = await kuroWebDav.ListRemoteAccountsAsync();
            RenderWebDavAccounts(remaining);
            ShowWebDavMessage(InfoBarSeverity.Success, $"已从 WebDAV 清除账号 {accountId} 的云端凭证。");
        }
        catch (Exception ex)
        {
            ShowWebDavMessage(InfoBarSeverity.Error, $"清除云端凭证失败：{ex.Message}");
        }
        finally { SetWebDavBusy(false); }
    }

    private static string FormatDeviceName(string source) => source switch
    {
        "pc" => "PC 端",
        "browser" => "浏览器扩展",
        "pocket" => "移动端",
        _ => string.IsNullOrWhiteSpace(source) ? "未知设备" : source
    };
}
