using System.Diagnostics;
using System.IO.Compression;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using IMao_WinUI.Helpers;

internal static class Program
{
    private static readonly List<string> Passed = [];
    private static Type PickerType = typeof(ResourcePackagePicker);

    [STAThread]
    private static int Main(string[] args)
    {
        var output = Path.GetFullPath(args.FirstOrDefault() ?? "out/resource-picker-tests");
        Directory.CreateDirectory(output);
        bool dialogs = !args.Contains("--abi-only", StringComparer.Ordinal);
        Exception? failure = null;
        try
        {
            int assemblyArgument = Array.IndexOf(args, "--assembly");
            if (assemblyArgument >= 0)
            {
                if (assemblyArgument + 1 >= args.Length) throw new ArgumentException("--assembly requires an application DLL path.");
                PickerType = Assembly.LoadFrom(Path.GetFullPath(args[assemblyArgument + 1]))
                    .GetType("IMao_WinUI.Helpers.ResourcePackagePicker", throwOnError: true)!;
            }
            CheckAbi();
            if (dialogs) CheckDialogs(output);
        }
        catch (Exception ex)
        {
            failure = ex;
            Console.Error.WriteLine(ex);
        }
        var report = new { passed = failure is null, architecture = RuntimeInformation.ProcessArchitecture.ToString(),
            processId = Environment.ProcessId, pickerAssembly = PickerType.Assembly.Location,
            nativeDialogsRequested = dialogs, assertions = Passed,
            failure = failure?.ToString() };
        File.WriteAllText(Path.Combine(output, "picker-regression.json"), JsonSerializer.Serialize(report, new JsonSerializerOptions { WriteIndented = true }));
        Console.WriteLine($"Resource package picker: {Passed.Count} assertions passed; {(failure is null ? "PASS" : "FAIL")}.");
        return failure is null ? 0 : 1;
    }

    private static void CheckAbi()
    {
        // Windows SDK commdlg.h: DWORD/WORD/pointer fields of OPENFILENAMEW.
        // This checks the actual application declaration; an independent good declaration would miss regressions.
        var type = PickerType.GetNestedType("OpenFileName", BindingFlags.NonPublic)
            ?? throw new InvalidOperationException("The application OPENFILENAMEW declaration was not found.");
        int size = Marshal.SizeOf(type); // The original StringBuilder field throws here before any dialog exists.
        Check(size == (IntPtr.Size == 8 ? 152 : 88), "OPENFILENAMEW native structure size");
        var fields = new (string Name, int X64, int X86)[]
        {
            ("StructSize", 0, 0), ("Owner", 8, 4), ("Instance", 16, 8), ("Filter", 24, 12),
            ("CustomFilter", 32, 16), ("MaxCustomFilter", 40, 20), ("FilterIndex", 44, 24),
            ("File", 48, 28), ("MaxFile", 56, 32), ("FileTitle", 64, 36), ("MaxFileTitle", 72, 40),
            ("InitialDirectory", 80, 44), ("Title", 88, 48), ("Flags", 96, 52),
            ("FileOffset", 100, 56), ("FileExtension", 102, 58), ("DefaultExtension", 104, 60),
            ("CustomData", 112, 64), ("Hook", 120, 68), ("TemplateName", 128, 72),
            ("Reserved", 136, 76), ("ReservedValue", 144, 80), ("FlagsEx", 148, 84)
        };
        foreach (var field in fields)
            Check(Marshal.OffsetOf(type, field.Name).ToInt32() == (IntPtr.Size == 8 ? field.X64 : field.X86), $"OPENFILENAMEW {field.Name} offset");
        Check(type.GetField("File")!.FieldType == typeof(nint), "Writable filename buffer is an explicit native pointer");
    }

    private static void CheckDialogs(string output)
    {
        var workingDirectory = Environment.CurrentDirectory;
        var fixtureRoot = Path.Combine(output, "中文 空格 " + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(fixtureRoot);
        var zipPath = Path.Combine(fixtureRoot, "地图 资源包 测试.zip");
        using (var archive = ZipFile.Open(zipPath, ZipArchiveMode.Create))
        using (var writer = new StreamWriter(archive.CreateEntry("fixture.txt").Open())) writer.Write("picker fixture only");

        Check(RunDialog(null) is null, "Real common dialog cancellation returns null");
        Check(Environment.CurrentDirectory == workingDirectory, $"Cancellation preserves the working directory (expected {workingDirectory}, actual {Environment.CurrentDirectory})");
        var selected = RunDialog(zipPath);
        Check(selected == zipPath, "Real common dialog returns the complete Unicode and spaces ZIP path");
        Check(Environment.CurrentDirectory == workingDirectory, $"File selection preserves the working directory (expected {workingDirectory}, actual {Environment.CurrentDirectory})");
        Check(RunDialog(null) is null, "A second cancellation works after a successful selection");
    }

    private static string? RunDialog(string? path)
    {
        nint owner = Native.CreateWindowEx(0, "STATIC", "ResourcePackagePicker regression owner", 0,
            0, 0, 0, 0, 0, 0, 0, 0);
        if (owner == 0) throw new InvalidOperationException($"Cannot create hidden test owner: {Marshal.GetLastWin32Error()}.");
        using var completed = new CancellationTokenSource();
        Exception? controllerFailure = null;
        bool observed = false;
        var controller = new Thread(() =>
        {
            nint dialog = 0;
            try
            {
                var timer = Stopwatch.StartNew();
                while (!completed.IsCancellationRequested && timer.Elapsed < TimeSpan.FromSeconds(15))
                {
                    dialog = FindOwnDialog();
                    if (dialog != 0 && Native.IsWindowVisible(dialog) && Native.IsWindowEnabled(Native.GetDlgItem(dialog, 1))) break;
                    Thread.Sleep(20);
                }
                if (dialog == 0) throw new TimeoutException("No native picker dialog appeared in this test process.");
                observed = true;
                // Own-process HWND checks are repeated before every interaction; other applications are untouched.
                RequireOwnWindow(dialog);
                Native.ShowWindowAsync(dialog, 0);
                if (path is not null)
                {
                    nint editor = FindFilenameEditor(dialog);
                    if (editor == 0) throw new InvalidOperationException("No native filename editor was found. " + DescribeOwnDialog(dialog));
                    RequireOwnWindow(editor);
                    if (Native.SendMessageTimeout(editor, 0x000C, 0, path, 2, 3000, out _) == 0) // WM_SETTEXT
                        throw new TimeoutException("Setting the test filename timed out.");
                    var enteredPath = new StringBuilder(32768);
                    if (Native.SendMessageTimeoutText(editor, 0x000D, enteredPath.Capacity, enteredPath, 2, 3000, out _) == 0) // WM_GETTEXT
                        throw new TimeoutException("Reading the test filename timed out.");
                    if (enteredPath.ToString() != path) throw new InvalidOperationException("Native filename editor did not retain the complete test path.");
                }
                RequireOwnWindow(dialog);
                Native.PostMessage(dialog, 0x111, path is null ? 2 : 1, 0); // WM_COMMAND IDCANCEL/IDOK.
                timer.Restart();
                while (!completed.IsCancellationRequested && timer.Elapsed < TimeSpan.FromSeconds(10)) Thread.Sleep(20);
                if (!completed.IsCancellationRequested) throw new TimeoutException("Native picker did not finish after its test action.");
            }
            catch (Exception ex)
            {
                controllerFailure = ex;
                if (dialog != 0 && IsOwnWindow(dialog)) Native.PostMessage(dialog, 0x111, 2, 0);
            }
        }) { IsBackground = true, Name = "Own-process native picker test controller" };
        controller.Start();
        string? result;
        try { result = (string?)PickerType.GetMethod("Pick", BindingFlags.Public | BindingFlags.Static)!.Invoke(null, [owner]); }
        finally
        {
            completed.Cancel();
            controller.Join(TimeSpan.FromSeconds(4));
            Native.DestroyWindow(owner);
        }
        if (controllerFailure is not null) throw new InvalidOperationException("Native dialog controller failed.", controllerFailure);
        Check(observed, path is null ? "Native cancel dialog appeared" : "Native selection dialog appeared");
        return result;
    }

    private static nint FindOwnDialog()
    {
        nint found = 0;
        Native.EnumWindows((window, _) =>
        {
            if (!IsOwnWindow(window)) return true;
            var className = new StringBuilder(80);
            Native.GetClassName(window, className, className.Capacity);
            if (className.ToString() != "#32770") return true;
            found = window;
            return false;
        }, 0);
        return found;
    }

    private static string DescribeOwnDialog(nint dialog)
    {
        RequireOwnWindow(dialog);
        var controls = new List<string>();
        Native.EnumChildWindows(dialog, (window, _) =>
        {
            if (!IsOwnWindow(window)) return true;
            var className = new StringBuilder(80);
            var text = new StringBuilder(1024);
            Native.GetClassName(window, className, className.Capacity);
            Native.SendMessageTimeoutText(window, 0x000D, text.Capacity, text, 2, 1000, out _);
            controls.Add($"{Native.GetDlgCtrlID(window)}:{className}:{text}");
            return true;
        }, 0);
        return string.Join(" | ", controls);
    }

    private static nint FindFilenameEditor(nint dialog)
    {
        RequireOwnWindow(dialog);
        nint found = 0;
        Native.EnumChildWindows(dialog, (window, _) =>
        {
            if (!IsOwnWindow(window)) return true;
            var className = new StringBuilder(80);
            Native.GetClassName(window, className, className.Capacity);
            int id = Native.GetDlgCtrlID(window);
            if (className.ToString() != "Edit" || (id != 0x47C && id != 0x480)) return true;
            found = window;
            return false;
        }, 0);
        return found;
    }

    private static bool IsOwnWindow(nint window)
    {
        Native.GetWindowThreadProcessId(window, out uint pid);
        return pid == Environment.ProcessId;
    }

    private static void RequireOwnWindow(nint window)
    {
        if (!IsOwnWindow(window)) throw new InvalidOperationException("Refusing to control a window outside the test process.");
    }

    private static void Check(bool condition, string name)
    {
        if (!condition) throw new InvalidOperationException(name);
        Passed.Add(name);
        Console.WriteLine("PASS " + name);
    }

    private static class Native
    {
        internal delegate bool EnumWindowProc(nint window, nint parameter);
        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "CreateWindowExW")]
        internal static extern nint CreateWindowEx(uint extendedStyle, string className, string windowName, uint style,
            int x, int y, int width, int height, nint parent, nint menu, nint instance, nint parameter);
        [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool DestroyWindow(nint window);
        [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool EnumWindows(EnumWindowProc callback, nint parameter);
        [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool EnumChildWindows(nint window, EnumWindowProc callback, nint parameter);
        [DllImport("user32.dll")] internal static extern int GetDlgCtrlID(nint window);
        [DllImport("user32.dll")] internal static extern uint GetWindowThreadProcessId(nint window, out uint processId);
        [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "GetClassNameW")]
        internal static extern int GetClassName(nint window, StringBuilder className, int maxCount);
        [DllImport("user32.dll")] internal static extern nint GetDlgItem(nint dialog, int id);
        [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool ShowWindowAsync(nint window, int command);
        [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool IsWindowVisible(nint window);
        [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool IsWindowEnabled(nint window);
        [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageTimeoutW")]
        internal static extern nint SendMessageTimeout(nint window, uint message, nint wParam, string lParam, uint flags, uint timeout, out nint result);
        [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageTimeoutW")]
        internal static extern nint SendMessageTimeoutText(nint window, uint message, nint wParam, StringBuilder lParam, uint flags, uint timeout, out nint result);
        [DllImport("user32.dll", EntryPoint = "PostMessageW")]
        [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool PostMessage(nint window, uint message, nint wParam, nint lParam);
    }
}
