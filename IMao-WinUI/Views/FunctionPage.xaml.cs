using IMao_WinUI.Helpers;
using IMao_WinUI.Services;
using IMao_WinUI.ViewModels;
using Microsoft.UI.Xaml.Controls;
using System.Collections.ObjectModel;
using System.Diagnostics;

namespace IMao_WinUI.Views;

class RouteName
{
    private readonly string routesFolderPath = UserDataPaths.SavedRoutes;

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
    private bool restoringConfiguration = true;

    public FunctionViewModel ViewModel { get; }

    public FunctionPage()
    {
        ViewModel = App.GetService<FunctionViewModel>();
        coreHost = App.GetService<CoreHostService>();
        InitializeComponent();
        ComboBox_RouteDataName.ItemsSource = routeName.GetAllRouteFilesName();
        RestoreConfiguration();
        Loaded += (_, _) => RestoreConfiguration();
    }

    private void RestoreConfiguration()
    {
        restoringConfiguration = true;
        var value = coreHost.Configuration;
        UpdateMinMapItemDataCycle.Value = value.MinMapUpdateCycle;
        UpdateMapItemDataCycle.Value = value.MapUpdateCycle;
        Setting_MinMapShowItem.IsOn = value.MinMapEnabled;
        Setting_MapShowItem.IsOn = value.MapEnabled;
        Setting_SetVisibleSavedPoints.IsOn = value.SavedPointsEnabled;
        ToggleSwitch_StatusBar.IsOn = value.StatusBarEnabled;
        restoringConfiguration = false;
    }

    private async Task ConfigureAsync(Func<Task<bool>> update)
    {
        if (restoringConfiguration) return;
        await update();
        RestoreConfiguration();
    }

    private async void UpdateMinMapItemDataCycle_ValueChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
    {
        if (e.NewValue < 16 || e.NewValue > 1000) return;
        await ConfigureAsync(() => coreHost.ConfigureAsync(minMapUpdateCycle: (int)e.NewValue));
    }

    private async void UpdateMapItemDataCycle_ValueChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
    {
        if (e.NewValue < 16 || e.NewValue > 1000) return;
        await ConfigureAsync(() => coreHost.ConfigureAsync(mapUpdateCycle: (int)e.NewValue));
    }

    private async void ToggleSwitch_MapShowItem(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (sender is ToggleSwitch toggle) await ConfigureAsync(() => coreHost.ConfigureAsync(mapEnabled: toggle.IsOn));
    }

    private async void ToggleSwitch_MinMapShowItem(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (sender is ToggleSwitch toggle) await ConfigureAsync(() => coreHost.ConfigureAsync(minMapEnabled: toggle.IsOn));
    }

    private async void ToggleSwitch_SetVisibleSavedPoints(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (sender is ToggleSwitch toggle) await ConfigureAsync(() => coreHost.ConfigureAsync(savedPointsEnabled: toggle.IsOn));
    }

    private async void ToggleSwitch_StatusBar_Toggled(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (sender is ToggleSwitch toggle) await ConfigureAsync(() => coreHost.ConfigureAsync(statusBarEnabled: toggle.IsOn));
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
            string routesPath = UserDataPaths.SavedRoutes;
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
        if (Directory.Exists(UserDataPaths.SavedRoutes)) _ = coreHost.LoadRoutesAsync();
    }

    private void Button_LoadOneRouteData(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (ComboBox_RouteDataName.SelectedItem is string selected && selected != "Empty") _ = coreHost.LoadRouteAsync(selected);
    }

    private void ComboBox_RouteDataName_DropDownOpened(object sender, object e) =>
        ComboBox_RouteDataName.ItemsSource = routeName.GetAllRouteFilesName();
}
