using IMao_WinUI.ViewModels;
using IMao_WinUI.Views.Controls;
using Microsoft.UI.Xaml.Controls;
namespace IMao_WinUI.Views;
public sealed partial class FilterPage : Page
{
    public FilterViewModel ViewModel { get; }
    public FilterControl Filter => FilterUi;
    public FilterPage()
    {
        ViewModel = App.GetService<FilterViewModel>();
        InitializeComponent();
    }
}
