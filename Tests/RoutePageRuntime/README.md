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

What it pins (23 assertions):

- The game-only entries are **gone** from the page (box/lasso/point/start tools, 加入可见点, 结束选点, the whole
  hand-drawn section, route naming, save/load buttons) and the saved-route list is a read-only `ListView`.
- The operations that do work without the map still dispatch the same commands with the same identities:
  `complete`/`skip`/`guide` carry key + routeId + profileId, `stop` carries routeId + profileId.
- The shared real-time-planning setting still follows the core snapshot and still writes back through
  `ConfigureAsync`.
- The page still renders at the 800×500 minimum window size, with 当前目标攻略 reachable.

Screenshots: `route-empty.png`, `route-active.png` (active route + saved routes), `route-800.png`.

It is a page-wiring and layout fixture. It does not replace native runtime, real persistence, real-game
controller or physical DPI testing, and it does not exercise the other five pages.
