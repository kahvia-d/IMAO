using System.Runtime.InteropServices;

namespace IMao_WinUI.Helpers;

/// <summary>
/// The native open/save dialogs, owned by one of our windows.
///
/// The WinRT pickers are not used anywhere in this shell: without package identity they need an
/// <c>IInitializeWithWindow</c> handshake, and the overlay can be running elevated, where the common
/// dialog is the one that still works. This is the same call the offline resource-package import has
/// always used, lifted out so a second and third caller do not grow their own copy of the struct.
/// </summary>
internal static class NativeFileDialog
{
    /// <summary>A filter string in the common-dialog form: "描述 (*.json)\0*.json\0\0".</summary>
    public static string? Open(nint owner, string filter, string title) =>
        Show(owner, filter, title, null, save: false);

    public static string? Save(nint owner, string filter, string title, string suggestedName) =>
        Show(owner, filter, title, suggestedName, save: true);

    private static string? Show(nint owner, string filter, string title, string? suggestedName, bool save)
    {
        string workingDirectory = Environment.CurrentDirectory;
        const int capacity = 32768; // OPENFILENAMEW.nMaxFile counts UTF-16 characters, including the terminator.
        // StringBuilder is not supported as a marshalled structure field. lpstrFile must
        // point to a writable buffer whose lifetime covers the entire modal dialog.
        nint fileBuffer = Marshal.AllocHGlobal(capacity * sizeof(char));
        try
        {
            Marshal.WriteInt16(fileBuffer, 0);
            if (!string.IsNullOrEmpty(suggestedName))
            {
                var suggestion = suggestedName.AsSpan();
                if (suggestion.Length >= capacity) suggestion = suggestion[..(capacity - 1)];
                Marshal.Copy(suggestion.ToArray(), 0, fileBuffer, suggestion.Length);
                Marshal.WriteInt16(fileBuffer, suggestion.Length * sizeof(char), 0);
            }
            var data = new OpenFileName
            {
                StructSize = (uint)Marshal.SizeOf<OpenFileName>(), Owner = owner,
                Filter = filter, FilterIndex = 1, File = fileBuffer, MaxFile = capacity, Title = title,
                // OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR, plus
                // OFN_OVERWRITEPROMPT when the answer is a file we are about to write.
                Flags = 0x00080000 | 0x00001000 | 0x00000800 | 0x00000008 | (save ? 0x00000002u : 0u)
            };
            bool accepted = save ? GetSaveFileName(ref data) : GetOpenFileName(ref data);
            if (accepted) return Marshal.PtrToStringUni(fileBuffer);
            uint error = CommDlgExtendedError();
            if (error != 0) throw new IOException($"无法打开文件选择窗口（{error:X}）。");
            return null;
        }
        finally
        {
            Marshal.FreeHGlobal(fileBuffer);
            // OFN_NOCHANGEDIR is ineffective for GetOpenFileName. Preserve relative-path
            // behavior after either selection or cancellation of the native dialog.
            Environment.CurrentDirectory = workingDirectory;
        }
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct OpenFileName
    {
        public uint StructSize;
        public nint Owner, Instance;
        [MarshalAs(UnmanagedType.LPWStr)] public string? Filter;
        public nint CustomFilter;
        public uint MaxCustomFilter, FilterIndex;
        public nint File;
        public uint MaxFile;
        public nint FileTitle;
        public uint MaxFileTitle;
        [MarshalAs(UnmanagedType.LPWStr)] public string? InitialDirectory;
        [MarshalAs(UnmanagedType.LPWStr)] public string? Title;
        public uint Flags;
        public ushort FileOffset, FileExtension;
        public nint DefaultExtension, CustomData, Hook, TemplateName, Reserved;
        public uint ReservedValue, FlagsEx;
    }

    [DllImport("comdlg32.dll", CharSet = CharSet.Unicode, EntryPoint = "GetOpenFileNameW", ExactSpelling = true)]
    [return: MarshalAs(UnmanagedType.Bool)] private static extern bool GetOpenFileName(ref OpenFileName data);
    [DllImport("comdlg32.dll", CharSet = CharSet.Unicode, EntryPoint = "GetSaveFileNameW", ExactSpelling = true)]
    [return: MarshalAs(UnmanagedType.Bool)] private static extern bool GetSaveFileName(ref OpenFileName data);
    [DllImport("comdlg32.dll")] private static extern uint CommDlgExtendedError();
}
