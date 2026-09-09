#nullable enable
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace IMao_WinUI.Core.Updates;

// The launcher owns the only long-lived handle: a launcher crash also closes its app and native children.
internal sealed class ProgramProcessJob : IDisposable
{
    private readonly SafeFileHandle? handle;
    public string Name { get; } = "IMao.ProgramJob." + Guid.NewGuid().ToString("N");
    public ProgramProcessJob()
    {
        if (!OperatingSystem.IsWindows()) return;
        handle = CreateJobObject(IntPtr.Zero, Name);
        if (handle.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error());
        var limits = new ExtendedLimits { Basic = new BasicLimits { LimitFlags = 0x2000 } }; // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if (!SetInformationJobObject(handle, 9, ref limits, (uint)Marshal.SizeOf<ExtendedLimits>()))
        { int error = Marshal.GetLastWin32Error(); handle.Dispose(); throw new Win32Exception(error); }
    }
    public static void JoinFromChild()
    {
        if (!OperatingSystem.IsWindows()) return;
        var name = Environment.GetEnvironmentVariable("IMAO_LAUNCH_JOB");
        if (name is null || !name.StartsWith("IMao.ProgramJob.", StringComparison.Ordinal)) throw new IOException("启动器进程保护信息缺失。");
        using var job = OpenJobObject(0x1, false, name); // JOB_OBJECT_ASSIGN_PROCESS
        if (job.IsInvalid || !AssignProcessToJobObject(job, Process.GetCurrentProcess().Handle)) throw new Win32Exception(Marshal.GetLastWin32Error(), "无法加入启动器进程组。");
    }
    public void Dispose() => handle?.Dispose();
    [StructLayout(LayoutKind.Sequential)] private struct BasicLimits
    {
        public long ProcessTime, JobTime; public uint LimitFlags; public UIntPtr MinimumWorkingSet, MaximumWorkingSet;
        public uint ActiveProcessLimit; public UIntPtr Affinity; public uint Priority, Scheduling;
    }
    [StructLayout(LayoutKind.Sequential)] private struct IoCounters { public ulong ReadOperations, WriteOperations, OtherOperations, ReadBytes, WriteBytes, OtherBytes; }
    [StructLayout(LayoutKind.Sequential)] private struct ExtendedLimits
    {
        public BasicLimits Basic; public IoCounters Io; public UIntPtr ProcessMemory, JobMemory, PeakProcessMemory, PeakJobMemory;
    }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern SafeFileHandle CreateJobObject(IntPtr attributes, string name);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern SafeFileHandle OpenJobObject(uint access, [MarshalAs(UnmanagedType.Bool)] bool inherit, string name);
    [DllImport("kernel32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool SetInformationJobObject(SafeFileHandle job, int infoClass, ref ExtendedLimits info, uint length);
    [DllImport("kernel32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool AssignProcessToJobObject(SafeFileHandle job, IntPtr process);
}
