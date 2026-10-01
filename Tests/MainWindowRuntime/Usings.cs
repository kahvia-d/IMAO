// The production project gets this from IMao-WinUI/Usings.cs. Linked files see the global usings of the
// project that compiles them, not of the project they came from, so the harness has to repeat it:
// without it, AppNotificationService cannot find WinUIEx's Window.ShowMessageDialogAsync/BringToFront.
global using WinUIEx;
