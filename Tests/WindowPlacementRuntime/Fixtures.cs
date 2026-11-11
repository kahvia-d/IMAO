global using WinUIEx;
namespace IMao_WinUI {
 internal static class App {
  public static bool RuntimeServicesInitialized => false;
  public static T GetService<T>() => throw new InvalidOperationException("Runtime services must not initialize in the placement fixture.");
 }
}
namespace IMao_WinUI.Services {
 internal sealed class MarkerGuideCoordinator { public void Dispose() {} }
 internal sealed class CoreHostService { public Task ShutdownAsync() => Task.CompletedTask; }
}
namespace IMao_WinUI.Helpers {
 internal static class UserDataPaths { public static string Root {get;set;} = ""; }
 internal static class FixtureExtensions { public static string GetLocalized(this string value) => "Placement runtime fixture"; }
 internal static class TitleBarHelper { public static void ApplySystemThemeToCaptionButtons() {} }
}
