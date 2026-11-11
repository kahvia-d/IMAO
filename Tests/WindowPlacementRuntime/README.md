# Real main-window placement lifecycle

Build with `./Tests/WindowPlacementRuntime/Build.ps1`, then run `out/window-placement-runtime/WindowPlacementRuntime.exe`.

This isolated WinUI fixture links the production MainWindow XAML/code, placement store and AtomicFile. Only unrelated application services, title localization, caption theme integration and the user-data root are fixtures. It opens its own windows and closes them automatically, without capture APIs or the native game core. The user-data root is a temporary directory and is removed afterward; no actual player preferences are touched.

The log is `%TEMP%/IMao-window-placement-runtime.log`. Checks cover first-run centering before activation, normal-close/recreation, maximize/minimize-close/recreation, canceled close, and unchanged executable-directory inventory. The pure `Tests/WindowPlacement` harness covers synthetic removed/negative monitors and DPI changes that require different physical hardware to reproduce end to end.
