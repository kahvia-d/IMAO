# Real WinUI guide-window integration harness

This executable links the production `MarkerGuideWindow`, `MarkerGuideCoordinator` and guide/route models. Only the CoreHost and detail service are replaced with narrow in-memory fakes. It creates actual WinUI windows on the current interactive desktop, then closes all test windows and exits. It never starts CoreHost, connects to the game, reads user progress, sends synthetic keyboard input, or accesses the network.

The assertions inspect the real `OverlappedPresenter`, `AppWindow.IsVisible`, native `WS_CAPTION` and `IsWindowVisible`. They also compare `ClientToScreen(0,0)` with `GetWindowRect` and DWM extended frame bounds: both measured top gaps must be zero. A title-bar flag alone does not establish that there is no visible nonclient strip. Controlled asynchronous replies exercise registration, local/online loading, authoritative route lookup, completion acknowledgement and marker event ordering. F8 and paging are injected at the native protocol event boundary; this does not test the global keyboard hook itself. `CompleteCurrentAsync` exercises the completion-button save path, while refresh and enlarge tests invoke the real controls' automation providers.

Build from the repository root in the configured Visual Studio developer environment. The Windows App SDK version matches the main application, and output stays outside the running application directory:

```powershell
. .\scripts\Enter-DevEnvironment.ps1
dotnet restore .\Tests\GuideWindowRuntime\GuideWindowRuntime.csproj -p:Platform=x64 -p:NuGetAudit=false --ignore-failed-sources
.\Tests\GuideWindowRuntime\Build.ps1
```

The build script locates desktop MSBuild and explicitly selects the repository's .NET SDK. `EnableMsixTooling=true` selects the Windows App SDK's bundled PRI tasks; `WindowsPackageType=None` still produces an unpackaged test executable. Disabling MSIX tooling incorrectly falls back to missing legacy Visual Studio PRI tasks on this machine.

Run only in an interactive Windows desktop session. The test briefly shows its own guide windows. This command starts only the test executable; its manifest uses `asInvoker` and requires no elevation:

```powershell
$guideTest = Start-Process -FilePath (Resolve-Path .\out\guide-window-runtime\GuideWindowRuntime.exe) -WindowStyle Hidden -PassThru
if (-not $guideTest.WaitForExit(60000)) {
    Stop-Process -Id $guideTest.Id
    throw 'Guide window integration tests timed out.'
}
$guideTest.Refresh()
Get-Content .\out\guide-window-runtime\guide-window-tests.log
if ($guideTest.ExitCode -ne 0) { throw 'Guide window integration tests failed.' }
```

The log contains one PASS line per scenario, real HWND/style evidence, and the final success line; exceptions result in a nonzero process exit code. This harness intentionally is not part of the default headless `Test-Runtime.ps1` workflow. A successful run establishes real-window behavior with controlled services, not real-game keyboard routing or external guide content availability.

Verified on 2026-09-08: 20 scenarios passed with process exit code 0 (19 real-window scenarios and one placement calculation group). They include physical game-left placement, 100/125/150/200 percent DPI calculations, negative coordinates, small game windows, image-page boundaries, stale paging events, image-to-empty refresh, and a visible enlarged-image failure message with the correct page number. Image fixtures are generated locally under the harness output directory; no image URL is fetched.

The earlier title-bar-only check missed a 9-pixel visible nonclient strip at 125% DPI despite `HasTitleBar=false` and `HasBorder=false`. Recorded API comparisons found 3 pixels after disabling resize, 9 after extending content into the title bar, and 8 after clearing frame style bits. The production `WM_NCCALCSIZE` handling reduces both actual top gaps to zero and keeps them zero after hide/reopen. Final native style is still `0x144C0000`; the geometry assertion, rather than that style alone, proves this fix. The comparison log is `out/guide-window-frame-comparison.log`.

For a visual review, the following optional mode shows one controlled two-image guide for 60 seconds, then closes its windows and exits. It uses a separate log and does not overwrite regression results:

```powershell
$guidePreview = Start-Process -FilePath (Resolve-Path .\out\guide-window-runtime\GuideWindowRuntime.exe) -ArgumentList '--preview' -WindowStyle Hidden -PassThru
$guidePreview.Id
Get-Content .\out\guide-window-runtime\guide-window-preview.log
```

The preview's buttons operate on its fake services. Global keyboard hooks and game positioning are still simulated; `--preview` never starts CoreHost or interacts with a real game.
