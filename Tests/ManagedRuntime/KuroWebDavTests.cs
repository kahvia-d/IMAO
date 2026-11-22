#nullable enable
using System.Collections.Concurrent;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using IMao_WinUI.Core.KuroSync;
using IMao_WinUI.Services;

/// <summary>
/// WebDAV 库街区凭证同步的离线单元测试（零外网依赖，使用内存 Mock WebDAV 服务）。
/// </summary>
internal static class KuroWebDavTests
{
    public static async Task RunAsync(string root, Action<bool, string> check)
    {
        // 1. 加密信封轮回、错误口令拒绝与 AAD 篡改检测
        var envelope = KuroWebDavCrypto.Encrypt(
            accountId: "10146974",
            version: 3,
            token: "tok-secret-value-12345",
            passphrase: "my-strong-passphrase",
            sourceDevice: "pc",
            displayName: "漂泊者大号",
            userInfoJson: "{\"userId\":\"10146974\",\"userName\":\"漂泊者大号\"}");
        string serialized = KuroWebDavCrypto.SerializeEnvelope(envelope);
        check(!serialized.Contains("tok-secret-value-12345", StringComparison.Ordinal),
            "WebDAV 信封 JSON 绝不泄露明文 Token");

        var parsed = KuroWebDavCrypto.ParseEnvelope(serialized);
        var decrypted = KuroWebDavCrypto.Decrypt(parsed, "my-strong-passphrase");
        check(decrypted.AccountId == "10146974" &&
              decrypted.Token == "tok-secret-value-12345" &&
              decrypted.UserInfoJson.Contains("漂泊者大号", StringComparison.Ordinal),
            "WebDAV 信封加解密轮回完整保留 Token 与用户信息");

        bool wrongPassRejected = false;
        try { KuroWebDavCrypto.Decrypt(parsed, "wrong-passphrase"); }
        catch (CryptographicException) { wrongPassRejected = true; }
        check(wrongPassRejected, "错误的同步口令会被 AES-GCM 认证标签拒绝");

        bool tamperedVersionRejected = false;
        try { KuroWebDavCrypto.Decrypt(parsed with { Version = 4 }, "my-strong-passphrase"); }
        catch (CryptographicException) { tamperedVersionRejected = true; }
        check(tamperedVersionRejected, "篡改信封版本号会触发 AAD 校验失败");

        // 1b. 跨端确定性测试向量：用 C# 解密浏览器扩展 webdav-crypto.js (WebCrypto) 产出的密文，确保两端逐字节互通
        var jsVectorEnvelope = new KuroWebDavEnvelope(
            1,
            "10146974",
            5,
            DateTimeOffset.Parse("2026-11-22T09:00:00.000Z"),
            "browser",
            "测试大号",
            new KuroWebDavKdfParams("PBKDF2-SHA256", 210_000, Convert.ToBase64String(new byte[] { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 })),
            new KuroWebDavCipherParams(
                "AES-256-GCM",
                Convert.ToBase64String(new byte[] { 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32 }),
                "9MguaWAyovtiDyTcom+m4yKCBAmnyg57icxNOZdqoJUk/fMhmyLuo4Bo99nF6X9MXVniGhQ3N9F6ge9ogoSPEV+1qcN4ktzCoByebsUMMMNmydEN/3Kb2CmgmuIt/reaJ0r0mssiF14E8PNt33pPmRk+1uHsKWBSETTC1jLbzKwgi6pCk2i1S6VUsTWWtyY+G4I9VHd8Lrx6xA+fUe0="));
        var jsVectorDecrypted = KuroWebDavCrypto.Decrypt(jsVectorEnvelope, "shared-passphrase-2026");
        check(jsVectorDecrypted.AccountId == "10146974" &&
              jsVectorDecrypted.Token == "cross-platform-token-xyz" &&
              jsVectorDecrypted.UserInfoJson == "{\"userId\":\"10146974\"}",
            "C# 端能准确解密浏览器扩展 WebCrypto 生成的确定性测试向量（跨端逐字节互通）");

        // 2. URL 校验：公网强制 HTTPS，局域网/本机允许 HTTP
        check(KuroWebDavCrypto.ValidateBaseUrl("https://dav.jianguoyun.com/dav").AbsoluteUri == "https://dav.jianguoyun.com/dav/",
            "WebDAV 地址自动规范化末尾斜杠");
        check(KuroWebDavCrypto.ValidateBaseUrl("http://192.168.1.10:5244/dav").AbsoluteUri == "http://192.168.1.10:5244/dav/",
            "局域网私有 IP 允许使用 http://");
        bool publicHttpRejected = false;
        try { KuroWebDavCrypto.ValidateBaseUrl("http://dav.jianguoyun.com/dav/"); }
        catch (ArgumentException) { publicHttpRejected = true; }
        check(publicHttpRejected, "公网地址禁止明文 http://");

        // 3. 便携配置文本编解码
        var bundle = new KuroWebDavConfigBundle("https://dav.jianguoyun.com/dav/", "user@example.com", "app-pass", "sync-pass");
        string encoded = KuroWebDavCrypto.EncodeConfigBundle(bundle);
        check(KuroWebDavCrypto.TryDecodeConfigBundle(encoded, out var decoded, out _) &&
              decoded.BaseUrl == bundle.BaseUrl &&
              decoded.Username == bundle.Username &&
              decoded.Password == bundle.Password &&
              decoded.Passphrase == bundle.Passphrase,
            "便携配置文本编解码完整保留四项配置");

        // 4. 设置持久化：密码与口令经 DPAPI 加密，绝不落明文
        string syncRoot = Path.Combine(root, "kuro-webdav-sync");
        var settingsStore = new KuroWebDavSettingsStore(syncRoot);
        check(!settingsStore.Load().Enabled, "WebDAV 同步默认处于关闭状态");
        var savedSettings = new KuroWebDavSettings(true, "https://dav.example.com/dav/", "alice", "app-secret-pw", "sync-secret-phrase");
        settingsStore.Save(savedSettings);
        string rawSettingsFile = File.ReadAllText(Path.Combine(syncRoot, "webdav-settings.json"));
        check(!rawSettingsFile.Contains("app-secret-pw", StringComparison.Ordinal) &&
              !rawSettingsFile.Contains("sync-secret-phrase", StringComparison.Ordinal),
            "本机 webdav-settings.json 中的密码与同步口令绝不落明文");
        var reloaded = settingsStore.Load();
        check(reloaded == savedSettings, "DPAPI 加密的 WebDAV 配置读取后与原值一致");

        // 5. Mock WebDAV 多账号上传/版本递增/拉取/双向同步与掉线续命
        var fakeServer = new InMemoryWebDavHandler();
        Func<KuroWebDavSettings, KuroWebDavClient> factory = s =>
            new KuroWebDavClient(s.BaseUrl, s.Username, s.Password, new HttpClient(fakeServer, disposeHandler: false));

        var vault = new KuroTokenVault(syncRoot);
        vault.Save(KuroTokenVault.AccountCredentialId("10146974"), "pc-token-v1", "10146974");
        var service = new KuroWebDavCredentialSyncService(syncRoot, factory);

        // 首次同步：把 PC 本机账号 10146974 推送到 WebDAV（v1）
        var first = await service.SyncAllAsync();
        check(first.Pushed == 1 && first.Pulled == 0 && first.RemoteAccounts.Count == 1 &&
              first.RemoteAccounts[0].AccountId == "10146974" && first.RemoteAccounts[0].Version == 1,
            "首次同步将本机账号推送到 WebDAV 并写入 v1");

        // 模拟另一台设备（如手机端）登录了大号新 Token（v2）和一个小号（v1）
        using (var mobileClient = factory(savedSettings))
        {
            var updatedMain = await mobileClient.UploadAsync("10146974", "mobile-token-v2", savedSettings.Passphrase, "pocket", "大号");
            var newAlt = await mobileClient.UploadAsync("20098765", "alt-token-v1", savedSettings.Passphrase, "pocket", "小号");
            check(updatedMain.Version == 2 && newAlt.Version == 1,
                "跨设备上传自动递增同一账号版本号，且与小号互相隔离");
        }

        // 模拟大号掉线自动续命：拒绝旧 Token 后自动从 WebDAV 拉取 v2
        bool refreshed = await service.TryRefreshAccountAsync("10146974", rejectedToken: "pc-token-v1");
        check(refreshed &&
              vault.TryRead(KuroTokenVault.AccountCredentialId("10146974"), out var refreshedCred) &&
              refreshedCred.Token == "mobile-token-v2",
            "掉线续命成功从 WebDAV 拉取 v2 新 Token 并更新本机 Vault");

        // 如果云端还是同一个被拒绝的 Token，不应误判为已刷新
        bool rejectedSame = await service.TryRefreshAccountAsync("10146974", rejectedToken: "mobile-token-v2");
        check(!rejectedSame, "云端 Token 与被拒绝 Token 相同时不会陷入无意义重试");

        // 再次全量同步：拉取小号 20098765 到本机，且大号保持不变
        var second = await service.SyncAllAsync();
        check(second.Pulled == 1 && second.Pushed == 0 &&
              vault.TryRead(KuroTokenVault.AccountCredentialId("20098765"), out var altCred) &&
              altCred.Token == "alt-token-v1",
            "多账号同步可将云端小号独立拉取到本机，不覆盖大号");

        // 删除云端小号不影响本机小号
        await service.DeleteRemoteAccountAsync("20098765");
        var remaining = await service.ListRemoteAccountsAsync();
        check(remaining.Count == 1 && remaining[0].AccountId == "10146974" &&
              vault.TryRead(KuroTokenVault.AccountCredentialId("20098765"), out _),
            "清除云端小号仅删除 WebDAV 文件，不破坏本机已有凭据");
    }

    /// <summary>纯内存 WebDAV HTTP 处理器，用于离线测试 MKCOL/PROPFIND/GET/PUT/DELETE。</summary>
    private sealed class InMemoryWebDavHandler : HttpMessageHandler
    {
        private readonly ConcurrentDictionary<string, string> files = new(StringComparer.Ordinal);
        private readonly ConcurrentDictionary<string, bool> dirs = new(StringComparer.Ordinal);

        protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            string path = request.RequestUri!.AbsolutePath;
            string method = request.Method.Method.ToUpperInvariant();
            if (method == "MKCOL")
            {
                bool existed = !dirs.TryAdd(path.TrimEnd('/') + "/", true);
                return new HttpResponseMessage(existed ? HttpStatusCode.MethodNotAllowed : HttpStatusCode.Created);
            }
            if (method == "PUT")
            {
                string body = request.Content is null ? "" : await request.Content.ReadAsStringAsync(cancellationToken);
                files[path] = body;
                return new HttpResponseMessage(HttpStatusCode.Created);
            }
            if (method == "GET")
            {
                if (!files.TryGetValue(path, out string? body))
                    return new HttpResponseMessage(HttpStatusCode.NotFound);
                return new HttpResponseMessage(HttpStatusCode.OK)
                {
                    Content = new StringContent(body, Encoding.UTF8, "application/json")
                };
            }
            if (method == "DELETE")
            {
                return new HttpResponseMessage(files.TryRemove(path, out _) ? HttpStatusCode.NoContent : HttpStatusCode.NotFound);
            }
            if (method == "PROPFIND")
            {
                string prefix = path.TrimEnd('/') + "/";
                var sb = new StringBuilder();
                sb.Append("<?xml version=\"1.0\" encoding=\"utf-8\"?><d:multistatus xmlns:d=\"DAV:\">");
                sb.Append($"<d:response><d:href>{prefix}</d:href></d:response>");
                foreach (string key in files.Keys.Where(k => k.StartsWith(prefix, StringComparison.Ordinal)))
                {
                    sb.Append($"<d:response><d:href>{key}</d:href></d:response>");
                }
                sb.Append("</d:multistatus>");
                return new HttpResponseMessage((HttpStatusCode)207)
                {
                    Content = new StringContent(sb.ToString(), Encoding.UTF8, "application/xml")
                };
            }
            return new HttpResponseMessage(HttpStatusCode.MethodNotAllowed);
        }
    }
}
