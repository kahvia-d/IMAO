using System.Runtime.InteropServices;

namespace IMao_WinUI.Helpers;

internal static class ResourcePackagePicker
{
    // Native common dialog also works when the overlay is run elevated.
    public static string? Pick(nint owner)
    {
        string workingDirectory = Environment.CurrentDirectory;
        const int capacity = 32768; // OPENFILENAMEW.nMaxFile counts UTF-16 characters, including the terminator.
        // StringBuilder is not supported as a marshalled structure field. lpstrFile must
        // point to a writable buffer whose lifetime covers the entire modal dialog.
        nint fileBuffer = Marshal.AllocHGlobal(capacity * sizeof(char));
        try
        {
            Marshal.WriteInt16(fileBuffer, 0);
            var data = new OpenFileName { StructSize = (uint)Marshal.SizeOf<OpenFileName>(), Owner = owner,
                Filter = "地图资源离线包 (*.zip)\0*.zip\0\0", FilterIndex = 1, File = fileBuffer, MaxFile = capacity,
                Title = "选择由维护者发布的地图资源离线包", Flags = 0x00080000 | 0x00001000 | 0x00000800 | 0x00000008 };
            if (GetOpenFileName(ref data)) return Marshal.PtrToStringUni(fileBuffer);
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
    [DllImport("comdlg32.dll")] private static extern uint CommDlgExtendedError();
}
