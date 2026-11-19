using Microsoft.UI.Xaml.Controls;

namespace IMao_WinUI.Views;

public sealed partial class WindowCloseDialog : ContentDialog
{
    public WindowCloseDialog() => InitializeComponent();

    public bool RememberChoice => RememberChoiceBox.IsChecked == true;
}
