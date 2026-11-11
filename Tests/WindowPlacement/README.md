# Main window placement regression

Run from the repository root after loading the development environment:

```powershell
. ./scripts/Enter-DevEnvironment.ps1
& $env:IMAO_DOTNET run --project Tests/WindowPlacement/WindowPlacement.csproj
```

The console harness uses production persistence and geometry code with a disposable user-data directory. It checks fresh-store reload, executable-directory isolation, atomic-file cleanup, mixed DPI scaling, removed/negative monitors, first-run centering, normal bounds across minimize/maximize, malformed/invalid data and reported write failures. It does not create a UI window or replace the real WinUI publish and launch/close gate.
