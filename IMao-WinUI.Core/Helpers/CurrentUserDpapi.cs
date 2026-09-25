using System.Runtime.InteropServices;
using System.Security.Cryptography;

namespace IMao_WinUI.Core.Helpers;

/// <summary>
/// Windows DPAPI scoped to the current user, which is how this program stores every credential it keeps
/// on disk. It lives here so there is a single P/Invoke surface: a second copy of this would be a second
/// place to get the blob ownership and sizing rules wrong.
/// </summary>
internal static class CurrentUserDpapi
{
    private const int CryptprotectUiForbidden = 0x1;

    [StructLayout(LayoutKind.Sequential)] private struct Blob { public int Size; public IntPtr Data; }

    [DllImport("crypt32.dll", SetLastError = true, CharSet = CharSet.Unicode)] private static extern bool CryptProtectData(ref Blob input,
        string description, IntPtr optionalEntropy, IntPtr reserved, IntPtr prompt, int flags, out Blob output);

    [DllImport("crypt32.dll", SetLastError = true, CharSet = CharSet.Unicode)] private static extern bool CryptUnprotectData(ref Blob input,
        IntPtr description, IntPtr optionalEntropy, IntPtr reserved, IntPtr prompt, int flags, out Blob output);

    [DllImport("kernel32.dll", SetLastError = true)] private static extern IntPtr LocalFree(IntPtr memory);

    public static byte[] Protect(byte[] data, string description) => Transform(data, input =>
    {
        if (!CryptProtectData(ref input, description, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, CryptprotectUiForbidden, out var output))
            throw new CryptographicException(Marshal.GetLastWin32Error());
        return output;
    });

    /// <summary>
    /// The description stays in the signature for symmetry with <see cref="Protect"/>: DPAPI records it in
    /// the blob but reports it back as an out parameter that is not worth a second allocation here, so it
    /// is deliberately not verified on the way back.
    /// </summary>
    public static byte[] Unprotect(byte[] data, string description) => Transform(data, input =>
    {
        if (!CryptUnprotectData(ref input, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, CryptprotectUiForbidden, out var output))
            throw new CryptographicException(Marshal.GetLastWin32Error());
        return output;
    });

    private static byte[] Transform(byte[] bytes, Func<Blob, Blob> transform)
    {
        IntPtr memory = Marshal.AllocHGlobal(bytes.Length);
        try
        {
            Marshal.Copy(bytes, 0, memory, bytes.Length);
            var output = transform(new Blob { Size = bytes.Length, Data = memory });
            try { var result = new byte[output.Size]; Marshal.Copy(output.Data, result, 0, result.Length); return result; }
            finally { if (output.Data != IntPtr.Zero) LocalFree(output.Data); }
        }
        finally { Marshal.FreeHGlobal(memory); }
    }
}
