using IMao_WinUI.Helpers;
using IMao_WinUI.Services;
using IMao_WinUI.ViewModels;
using Microsoft.UI.Xaml.Controls;
using System.Collections.ObjectModel;
using System.Diagnostics;

namespace IMao_WinUI.Views;

class RouteName
{
    private readonly string routesFolderPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "SavedRoutes");

    public ObservableCollection<string> GetAllRouteFilesName()
    {
        try
        {
            string[] paths = Directory.GetFiles(routesFolderPath, "*.json");
            return paths.Length == 0
                ? new ObservableCollection<string> { "Empty" }
                : new ObservableCollection<string>(paths.Select(Path.GetFileNameWithoutExtension));
        }
        catch (Exception exception)
        {
            Debug.WriteLine($"读取路线失败：{exception.Message}");
            return new ObservableCollection<string> { "Empty" };
        }
    }
}

public sealed partial class FunctionPage : Page
{
    private readonly RouteName routeName = new();
    private readonly CoreHostService coreHost;

    public FunctionViewModel ViewModel { get; }

    public FunctionPage()
    {
        ViewModel = App.GetService<FunctionViewModel>();
        coreHost = App.GetService<CoreHostService>();
        InitializeComponent();
        ComboBox_RouteDataName.ItemsSource = routeName.GetAllRouteFilesName();
        ToggleSwitch_StatusBar.IsOn = RuntimePreferences.StatusBarEnabled;
    }

    private void UpdateMinMapItemDataCycle_ValueChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
    {
        // WinUI raises ValueChanged while the control is being constructed, before
        // the XAML value is applied.  Do not send that transient zero to CoreHost.
        if (e.NewValue < 16 || e.NewValue > 1000) return;
        _ = coreHost.ConfigureAsync(minMapUpdateCycle: (int)e.NewValue);
    }

    private void UpdateMapItemDataCycle_ValueChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
    {
        // See the matching minimap handler above.
        if (e.NewValue < 16 || e.NewValue > 1000) return;
        _ = coreHost.ConfigureAsync(mapUpdateCycle: (int)e.NewValue);
    }

    private void ToggleSwitch_MapShowItem(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (sender is ToggleSwitch toggle) _ = coreHost.ConfigureAsync(mapEnabled: toggle.IsOn);
    }

    private void ToggleSwitch_MinMapShowItem(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (sender is ToggleSwitch toggle) _ = coreHost.ConfigureAsync(minMapEnabled: toggle.IsOn);
    }

    private void ToggleSwitch_SetVisibleSavedPoints(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (sender is ToggleSwitch toggle) _ = coreHost.ConfigureAsync(savedPointsEnabled: toggle.IsOn);
    }

    private void ToggleSwitch_StatusBar_Toggled(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (sender is not ToggleSwitch toggle) return;
        RuntimePreferences.StatusBarEnabled = toggle.IsOn;
        _ = coreHost.ConfigureAsync(statusBarEnabled: toggle.IsOn);
    }

    private void Button_SavedRouteJsonName_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        string content = TextBox_SavedRouteJsonName.Text;
        if (!String.IsNullOrWhiteSpace(content)) _ = coreHost.SetRouteNameAsync(content);
    }

    private void Button_OpenRoutesFolder_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        try
        {
            string routesPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "SavedRoutes");
            if (Directory.Exists(routesPath))
            {
                Process.Start(new ProcessStartInfo(routesPath) { UseShellExecute = true, Verb = "open" });
            }
        }
        catch (Exception exception)
        {
            Debug.WriteLine($"打开路线目录失败：{exception.Message}");
        }
    }

    private void Button_LoadRoutesData_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (Directory.Exists(Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "SavedRoutes"))) _ = coreHost.LoadRoutesAsync();
    }

    private void Button_LoadOneRouteData(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (ComboBox_RouteDataName.SelectedItem is string selected && selected != "Empty") _ = coreHost.LoadRouteAsync(selected);
    }

    private void ComboBox_RouteDataName_DropDownOpened(object sender, object e) =>
        ComboBox_RouteDataName.ItemsSource = routeName.GetAllRouteFilesName();
}
