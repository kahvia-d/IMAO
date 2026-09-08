# Main WinUI runtime verification

This isolated executable links the production Shell, all six pages, navigation services, view models, map catalog model and theme. CoreHost, controller status and local filter storage are in-memory fixtures. It never starts the native core, reads the user's controller or writes the user's configuration.

Build from the repository root:

```powershell
./Tests/MainWindowRuntime/Build.ps1
```

Run `out/main-window-runtime/MainWindowRuntime.exe`. It briefly activates its own window and exits. Do not run alongside another test which depends on the foreground window.

The suite checks:

- All six destinations at 1120×780 and 800×500, using the real Frame and footer navigation selection.
- No horizontal page scrolling, bounded filter/log viewports and virtualization with 600 filter rows.
- Filtering and access to selection, route guide and shortcut-saving controls at the minimum size.
- Shared settings/route real-time planning switches, external configuration changes and rejected-save rollback.
- Exact marker/profile/route identity in the route completion command.
- Actual active route and target data in the overview.
- Restored window bounds and physical-pixel clamping on a negative-coordinate monitor with a 200% sized window.
- Shared filter controls with character/weapon shortcuts, material/acronym search, selected-only views, exact current-result batch changes and gamepad access to virtualized items.
- Atomic local failure rollback, visible synchronization failures, offline edits, latest-state replay on reconnect and rejection of acknowledgements from older revisions/connections.
- Reloaded controls observing the shared saved selection and the compact 300 DIP content layout used by the map tool dock.

Use `--test-filter-sharing` to run only the shared filter checks. They use a dedicated catalog fixture and an in-memory persistence delegate; successful UI wiring does not substitute for the existing local-storage tests. The two additional screenshots are `shared-filter-controls.png` and `800-filter-dock-compact.png`.

Output: `out/main-window-runtime/ui-tests.log`, twelve default page PNGs and three minimum-size action PNGs. Screenshots wait 650 ms after navigation so the default WinUI transition is finished. The intentionally absent catalog assets exercise a visible filter warning; they do not indicate missing production assets.

The fixture tests page wiring, layout and bounded window geometry. They do not replace native runtime, actual persistence, real-game controller or physical monitor DPI-switch testing.
