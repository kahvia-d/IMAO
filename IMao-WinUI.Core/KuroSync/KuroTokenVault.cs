using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace IMao_WinUI.Core.KuroSync;

public sealed record KuroCredential(string Token, DateTimeOffset SavedAt);

public sealed class KuroTokenVault
{
    private readonly string credentialsDirectory;

    public KuroTokenVault(string root) => credentialsDirectory = Path.Combine(root ?? throw new ArgumentNullException(nameof(root)), "credentials");

    public void Save(string profileId, string token)
    {
        ValidateProfile(profileId);
        if (string.IsNullOrWhiteSpace(token) || token.Length > 16 * 1024) throw new ArgumentException("无效的库街区登录凭据。", nameof(token));
        Directory.CreateDirectory(credentialsDirectory);
        var record = new StoredCredential(Convert.ToBase64String(Protect(Encoding.UTF8.GetBytes(token))), DateTimeOffset.UtcNow);
        string path = Path.Combine(credentialsDirectory, profileId + ".json");
        string temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        File.WriteAllText(temporary, JsonSerializer.Serialize(record));
        File.Move(temporary, path, overwrite: true);
    }

    public bool TryRead(string profileId, out KuroCredential credential)
    {
        credential = default!;
        try
        {
            ValidateProfile(profileId);
            string path = Path.Combine(credentialsDirectory, profileId + ".json");
            if (!File.Exists(path)) return false;
            var stored = JsonSerializer.Deserialize<StoredCredential>(File.ReadAllText(path));
            if (stored is null || string.IsNullOrWhiteSpace(stored.Ciphertext)) return false;
            credential = new(Encoding.UTF8.GetString(Unprotect(Convert.FromBase64String(stored.Ciphertext))), stored.SavedAt);
            return credential.Token.Length > 0;
        }
        catch (Exception error) when (error is ArgumentException or IOException or UnauthorizedAccessException or JsonException or CryptographicException)
        {
            return false;
        }
    }

    public void Delete(string profileId)
    {
        ValidateProfile(profileId);
        string path = Path.Combine(credentialsDirectory, profileId + ".json");
        if (File.Exists(path)) File.Delete(path);
    }

    private sealed record StoredCredential(string Ciphertext, DateTimeOffset SavedAt);

    private static void ValidateProfile(string profileId)
    {
        if (string.IsNullOrWhiteSpace(profileId) || profileId.Length > 96 || profileId.Any(c => !char.IsAsciiLetterOrDigit(c) && c is not '-' and not '_'))
            throw new ArgumentException("无效的同步档案。", nameof(profileId));
    }

    private static byte[] Protect(byte[] data) => Dpapi.Protect(data, "IMao.KuroSync.v1");
    private static byte[] Unprotect(byte[] data) => Dpapi.Unprotect(data, "IMao.KuroSync.v1");

    private static class Dpapi
    {
        [StructLayout(LayoutKind.Sequential)] private struct Blob { public int Size; public IntPtr Data; }
        [DllImport("crypt32.dll", SetLastError = true, CharSet = CharSet.Unicode)] private static extern bool CryptProtectData(ref Blob input,
            string description, IntPtr optionalEntropy, IntPtr reserved, IntPtr prompt, int flags, out Blob output);
        [DllImport("crypt32.dll", SetLastError = true, CharSet = CharSet.Unicode)] private static extern bool CryptUnprotectData(ref Blob input,
            IntPtr description, IntPtr optionalEntropy, IntPtr reserved, IntPtr prompt, int flags, out Blob output);
        [DllImport("kernel32.dll", SetLastError = true)] private static extern IntPtr LocalFree(IntPtr memory);
        private const int CryptprotectUiForbidden = 0x1;

        public static byte[] Protect(byte[] data, string description) => Transform(data, input =>
        {
            if (!CryptProtectData(ref input, description, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, CryptprotectUiForbidden, out var output))
                throw new CryptographicException(Marshal.GetLastWin32Error());
            return output;
        });

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
}
