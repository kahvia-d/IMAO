# Route page (FunctionPage) runtime verification

This isolated executable renders the **production** route page — `IMao-WinUI/Views/FunctionPage.xaml` plus its
code-behind are linked in verbatim — with only the services it calls replaced by fixtures. It never starts the
native core, never opens the game and never touches the user's configuration.

It exists because the older `Tests/MainWindowRuntime` harness links *all six* pages and has not compiled since
2026-09-27 (its project file does not pull in the KuroSync types `SettingsPage`/`UsageGuidePage` now need), so
the route page had no runnable UI check of its own.

Build (the repository .NET SDK and the VS MSBuild are both required):

```powershell
$env:MSBuildSDKsPath        = '<repo>\tools\dotnet-sdk-8.0.424\sdk\8.0.424\Sdks'
$env:DOTNET_MSBUILD_SDK_RESOLVER_CLI_DIR = '<repo>\tools\dotnet-sdk-8.0.424'
$env:MSBuildEnableWorkloadResolver = 'false'
$env:NUGET_PACKAGES = '<repo>\third_party\nuget-packages'
$env:USERPROFILE    = '<repo>\third_party\dotnet-user-profile'
$env:APPDATA        = "$env:USERPROFILE\AppData\Roaming"
$env:LOCALAPPDATA   = "$env:USERPROFILE\AppData\Local"
& 'C:\VSBuildTools-Current\MSBuild\Current\Bin\MSBuild.exe' Tests\RoutePageRuntime\RoutePageRuntime.csproj `
    /restore /t:Build /p:Configuration=Release /p:Platform=x64 /p:NuGetAudit=false /m
```

Run `out/route-page-runtime/RoutePageRuntime.exe`. It shows its own window briefly, writes
`out/route-page-runtime/route-page-tests.log` and exits with the assertion count.

What it pins (62 assertions):

- The page is only the operation guide plus the route list: every game-only entry is gone (the selection
  tools, 加入可见点, hand-drawing, save/load, and now also complete/skip/pause/stop, generate/activate and
  the real-time-planning switch), and the saved-route list is a `ListView`.
- Each row carries the point-type icons the way the in-game list does: a real PNG from
  `Assets/KuroMap/icons` is handed to the row the same way the core hands over `icon-manifest.json` paths,
  and the test asserts the row produced an `Image` whose source is a **decoded** `BitmapImage` with pixels,
  that the type name sits beside it, and that the path text does **not** leak into the row. The fallback
  (unreadable path → the path text, empty type table → 自由点（无类型）) is pinned through the converter
  directly.
- The row for the route being followed carries the dot. The fixture deliberately leaves `CurrentRoute`
  empty on the snapshot, so the dot is proven to come from `Active` alone.
- 开始指引 sends `switch` with the route id, the profile, `start: true` and the map context
  (`expectedSceneId`/`expectedGeneration`) - the same command the in-game list sends when a row is picked.
  A corrupt row cannot be switched to.
- The page renders at the 800×500 minimum window size.

Screenshots: `route-empty.png`, `route-active.png` (the route list with icons and the ● row), `route-800.png`.

It is a page-wiring and layout fixture. It does not replace native runtime, real persistence, real-game
controller or physical DPI testing, and it does not exercise the other five pages.

