# Map tools window runtime regression

This independent WinUI application links the production `MapToolsWindow`,
`MapToolsController`, `FilterControl`, input interpreter, window chrome,
activation and return helpers. It launches a separate copy of itself as a
test-owned game window, so foreground transfers cross a real process boundary.
It never launches or changes the user's game. Filter persistence uses an injected
in-memory store and a generated fixture catalog; the un-injected storage stubs
throw if called.

Build from the repository root:

```powershell
.\Tests\MapToolsRuntime\Build.ps1
```

Run `out\map-tools-runtime\MapToolsRuntime.exe` on the interactive desktop.
`.\Tests\MapToolsRuntime\Run.ps1` starts it with private local app data and prints
the resulting assertions.
In a desktop-isolated automation sandbox this requires the approved interactive
desktop permission: otherwise `GetForegroundWindow()` can be zero and the test
will correctly stop at its bootstrap, before testing tools activation. Coordinate
with other UI tests and let this executable own its test windows until it exits.
The harness restores focus only among its own source/tools/coordination windows.

Evidence goes to `out/map-tools-runtime/evidence-<timestamp>` and successful runs
update `out/map-tools-runtime/latest-results.txt`. The process exits nonzero on
the first failed assertion. Screenshots capture the actual displayed production
windows and are paired with an entire test-game screenshot.

Coverage includes:

- 1280×720 and 800×500 physical source sizes, animated input gating, physical
  bounds/launcher anchoring, and absence of a white native title strip;
- all 17 active-route/editing actions, scroll reachability, nested filter toolbar
  scrolling, real Chinese TextBox search, and shared immediate filter selection;
- opening and keeping the panel without a controller, disconnected samples during
  mouse canvas selection, native result acknowledgment and preserved selection;
- B through subtool, home and confirmed cross-process return; stopping and
  restarting the fake core; guide handoff source lifetime longer than five seconds;
- delayed route command/enable replies across timer ticks, delayed registration
  after another window acquires focus, and an old unregister reply arriving during
  a replacement session's command.

Boundaries: the CoreHost IPC transport and route computation are faked. Native
canvas geometry, marker matching, actual game focus policy, real guide content,
and a physical XInput device are separate integration checks. Screen scale is the
current desktop DPI, not a simulated claim of testing every DPI. Small windows
use real scroll viewports; a passing test means every tested action can be brought
fully into view, not that all actions fit on the first screen.
