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

What it pins (149 assertions):

- Sorting uses an in-window pointer gesture rather than WinUI system drag/drop, which throws
  `Drag start failed` in the elevated production app. The fixture pins system drag/drop off,
  stationary source rows, insertion-boundary preview, deferred state pushes, release-only persistence, cancellation, profile
  changes, save rejection recovery and selection preservation. All-collections and batch modes
  also refuse the internal gesture. These checks invoke the production gesture helpers; physical
  mouse capture and elevated in-game operation still need a manual test.
- Drag feedback highlights the source route, marks its insertion position, and shows its name
  and ordinal. The overlay follows first/last positions, does not intercept pointer input, and
  disappears after save or cancellation. Real-control PNGs `route-drag-start.png`,
  `route-drag-moved.png`, and `route-drag-last.png` cover its rendering.
- Release inserts the source into the chosen boundary, preserving other rows' relative order.
  Intermediate/end insertions account for removing the source. Composition transitions move the
  source and displaced rows over 240 ms, with the source above the other rows; runtime checks
  verify the transition stays active and state pushes cannot rebuild containers mid-animation.
  The system animation preference is respected.

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
- **Nothing in a row is empty but still takes a line.** The point-type placeholder and the badge row are
  alternatives: a `TextBlock` with `Text=""` still occupies a whole line in WinUI, so an always-visible
  placeholder put a blank row inside every route that has kinds (44px instead of 22px). Found from a
  player screenshot, 2026-10-01.
- **开始指引 is not on this page.** Starting a route happens in the game's own route list; the desktop
  keeps the organisation (collections, rename, delete, import/export). The fixture asserts the button is
  gone and that the list can still be selected, because 删除所选路线 works off that selection.
- **Every action button fits its own label** — checked at the default width and again at the 800px
  minimum, which required pinning `host.Width` as well as the window: resizing only the window leaves the
  page laid out at its old width and tests nothing. The rows are `Auto`-column `Grid`s rather than
  toolkit `WrapPanel`s, because a WrapPanel that is nearly full squeezes its last child instead of
  wrapping — that is what pushed the ellipsis of 导出当前合集… onto the border in the player's window.
- **Collections**: the bar is built from the snapshot, each chip carries its route count, clicking one
  sends `collectionCurrent` with that id and narrows the list to it, and 全部 widens the list back to
  every collection **without sending anything** - it is a way of looking, not a place to save into.
  The rename and delete buttons stand down while 全部 is showing, because there is no single collection
  they could act on.
- **Batch mode**: 批量… reveals the bar and turns every row into a checkbox, the count is reported as
  boxes are ticked, and 导出所选 sends `export` with exactly the ticked ids - not the unticked ones,
  and not the corrupt one - to the path the (substituted) file dialog returned. The single-row actions
  stand down while the bar is up.
- **Import**: the two dialogs are built but not shown, so the branches can be asserted without a modal
  window. A routes package asks one question and has no 覆盖/新建 choice; a collection package colliding
  with an existing name is the only case offering 覆盖 / 新建, and its text states how many routes would
  be replaced. Primary/Secondary/None map to `collectionOverwrite`/`collectionNew`/`routes`/nothing.
  (This assertion found a real bug: a routes package was being mapped to "create a new collection".)
- The page renders at the 800×500 minimum window size.

Screenshots: `route-empty.png`, `route-active.png` (the route list with icons and the ● row), `route-800.png`.

Sorting regressions check that the original containers slide before committing order, then update
the same ItemsSource with Move without replacing route records. Subsequent equivalent snapshots
retain the committed row containers. Snapshots allocate fresh records and icon arrays, matching IPC.
The fixture also covers an icon-bearing last row clipped by the viewport and delayed reorder replies
without disabling the list. Real metadata changes still render, and profile changes reset selection.

Run with `--animation-frames` to capture the fixture window before, during and after the compositor
animation (`route-motion-before/middle/after.png`). This opt-in mode checks that its own window is
foreground before capturing; the page is shifted only in the fixture to fit the list into the window.
These captures include compositor motion, unlike the usual RenderTargetBitmap layout screenshots.

Batch export naming checks the actual save-dialog suggestion: a single route uses its name;
multiple routes use the first exportable name plus a count suffix. Cancellation preserves selection,
unnamed routes use their IDs, invalid Windows filename characters and device names are made safe,
and corrupt routes do not contribute to the name/count. The user's chosen final path still wins.

It is a page-wiring and layout fixture. It does not replace native runtime, real persistence, real-game
controller or physical DPI testing, and it does not exercise the other five pages. The two file dialogs are
the one thing it cannot click: `FunctionPage.OpenBundlePath` / `SaveBundlePath` substitute them, and in
production the page still calls the native common dialog.

