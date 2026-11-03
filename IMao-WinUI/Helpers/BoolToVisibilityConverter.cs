using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Data;

namespace IMao_WinUI.Helpers;

/// <summary>
/// Shows a control when a flag is set, and hides it when it is not. The flag lives in a model that
/// the console test harness also compiles, so the model cannot return a WinUI <see cref="Visibility"/>
/// itself; the conversion happens here instead. Pass "invert" to show when the flag is *off*, which
/// is how one row carries both its normal and its batch-mode form.
/// </summary>
public sealed class BoolToVisibilityConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object parameter, string language)
    {
        var visible = value is bool flag && flag;
        if (parameter is string text && text.Equals("invert", StringComparison.OrdinalIgnoreCase)) visible = !visible;
        return visible ? Visibility.Visible : Visibility.Collapsed;
    }

    public object ConvertBack(object value, Type targetType, object parameter, string language) =>
        throw new NotSupportedException();
}

/// <summary>
/// 全局悬浮消息提示中心：统一将错误、警告与操作提示以顶部悬浮栏形式展示，
/// 不占用页面文档流空间；同一时刻仅保留最新一条提示，支持鼠标悬停暂停倒计时。
/// </summary>
public static class AppToast
{
    private static Action<string, InfoBarSeverity, string?>? presenter;

    // 由 ShellPage 注册全局悬浮提示展示器
    public static void RegisterPresenter(Action<string, InfoBarSeverity, string?>? handler)
    {
        presenter = handler;
    }

    // 弹出或刷新全局悬浮提示（新提示会覆盖旧提示并重置 4 秒自动消失计时）
    public static void Show(string message, InfoBarSeverity severity = InfoBarSeverity.Informational, string? title = null)
    {
        if (string.IsNullOrWhiteSpace(message)) return;
        presenter?.Invoke(message.Trim(), severity, string.IsNullOrWhiteSpace(title) ? null : title.Trim());
    }
}
