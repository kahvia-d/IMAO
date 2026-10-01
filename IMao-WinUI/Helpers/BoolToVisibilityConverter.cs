using Microsoft.UI.Xaml;
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
