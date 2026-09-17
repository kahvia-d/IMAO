using IMao_WinUI.Core.KuroSync;
using System.Text.Json;

internal static class KuroSyncTests
{
    public static void Run(string root, Action<bool, string> check)
    {
        string vaultRoot = Path.Combine(root, "kuro-sync");
        var vault = new KuroTokenVault(vaultRoot);
        vault.Save("kuro_12345", "secret-token");
        check(vault.TryRead("kuro_12345", out var credential) && credential.Token == "secret-token", "sync credential round trip is scoped to the profile");
        check(!File.ReadAllText(Path.Combine(vaultRoot, "credentials", "kuro_12345.json")).Contains("secret-token"), "sync credential token is never stored as plaintext");
        check(!vault.TryRead("kuro_99999", out _), "sync credential cannot cross account profiles");

        var baseline = new Dictionary<string, bool> { ["a"] = true, ["b"] = false, ["c"] = false };
        var local = new Dictionary<string, bool> { ["a"] = false, ["b"] = false, ["c"] = true };
        var remote = new Dictionary<string, bool> { ["a"] = true, ["b"] = true, ["c"] = false };
        var plan = KuroProgressMerge.Plan(baseline, local, remote);
        check(plan.Uploads.SequenceEqual(new[] { new KuroPointOperation("a", false), new KuroPointOperation("c", true) }), "local completion and cancellation become explicit uploads");
        check(plan.Downloads.SequenceEqual(new[] { new KuroPointOperation("b", true) }), "remote-only change is downloaded");

        var firstSync = KuroProgressMerge.Plan(null, new Dictionary<string, bool> { ["a"] = true }, new Dictionary<string, bool> { ["b"] = true });
        check(firstSync.RequiresInitialChoice, "missing baseline never silently merges account progress");

        check(KuroNativeBridgeProtocol.TryValidate(new KuroNativeBridgeRequest(1, "storeCredential", "kuro_12345", "secret-token"), out _),
            "native bridge accepts a bounded credential transfer");
        check(!KuroNativeBridgeProtocol.TryValidate(new KuroNativeBridgeRequest(2, "storeCredential", "kuro_12345", "secret-token"), out _),
            "native bridge rejects unknown protocol versions");
        check(!KuroNativeBridgeProtocol.TryValidate(new KuroNativeBridgeRequest(1, "runCommand", "kuro_12345", "secret-token"), out _),
            "native bridge rejects arbitrary commands");

        // The bridge decodes the extension's JSON verbatim, so the wire format is
        // part of the contract: camelCase field names and a clean end of stream.
        check(KuroNativeBridgeProtocol.TryValidate(KuroNativeBridgeProtocol.Parse(
                "{\"version\":1,\"type\":\"storeCredential\",\"profileId\":\"kuro_12345\",\"token\":\"secret-token\"}"), out _),
            "native bridge accepts the extension camelCase wire format");
        check(!KuroNativeBridgeProtocol.TryValidate(KuroNativeBridgeProtocol.Parse(
                "{\"version\":1,\"type\":\"runCommand\",\"profileId\":\"kuro_12345\",\"token\":\"secret-token\"}"), out var commandError) &&
            commandError == "unsupported-request", "native bridge rejects arbitrary commands in the extension wire format");

        using (var framed = new MemoryStream())
        {
            KuroNativeMessageFraming.WriteAsync(framed, new { accepted = true }).GetAwaiter().GetResult();
            framed.Position = 0;
            var decoded = KuroNativeMessageFraming.TryReadAsync(framed).GetAwaiter().GetResult();
            check(decoded is not null && decoded.Contains("\"accepted\":true"), "native message framing round trips a bounded response");
            check(KuroNativeMessageFraming.TryReadAsync(framed).GetAwaiter().GetResult() is null,
                "native message framing reports the end of the stream");
        }
        using (var truncated = new MemoryStream([1, 2, 3]))
            check(KuroNativeMessageFraming.TryReadAsync(truncated).GetAwaiter().GetResult() is null,
                "native message framing treats a truncated header as the end of the stream");
        using (var oversized = new MemoryStream([0xFF, 0xFF, 0xFF, 0x7F]))
        {
            bool rejected = false;
            try { KuroNativeMessageFraming.TryReadAsync(oversized).GetAwaiter().GetResult(); }
            catch (InvalidDataException) { rejected = true; }
            check(rejected, "native message framing rejects an oversized message");
        }

        var response = new HttpResponseMessage(System.Net.HttpStatusCode.OK)
        {
            Content = new StringContent("{\"code\":200,\"data\":[\"one\",\"two\"]}")
        };
        var handler = new KuroHttpHandler(response);
        using var client = new KuroMapProgressClient(new HttpClient(handler));
        var completed = client.GetCompletedIdsAsync("secret-token", 8, "device-test").GetAwaiter().GetResult();
        check(completed.SetEquals(["one", "two"]), "remote completed point ids are read from the documented Kuro API");
        check(handler.Request!.Headers.GetValues("token").Single() == "secret-token" && handler.Request.Headers.GetValues("state_id").Single() == "8",
            "remote progress request uses token and scene headers without query credential leakage");

        var statesHandler = new KuroHttpHandler(new HttpResponseMessage(System.Net.HttpStatusCode.OK)
        {
            Content = new StringContent("{\"code\":200,\"data\":{\"state\":[{\"id\":8,\"name\":\"瑝珑\"},{\"id\":905,\"name\":\"隐海试验场\"}]}}")
        });
        using var statesClient = new KuroMapProgressClient(new HttpClient(statesHandler));
        var states = statesClient.GetStatesAsync().GetAwaiter().GetResult();
        check(states.Count == 2 && states[0].StateId == 8 && states[1].Name == "隐海试验场",
            "map regions come from the published state selection");
        var brokenHandler = new KuroHttpHandler(new HttpResponseMessage(System.Net.HttpStatusCode.OK)
        {
            Content = new StringContent("{\"code\":500,\"data\":{}}")
        });
        using var brokenClient = new KuroMapProgressClient(new HttpClient(brokenHandler));
        bool statesRejected = false;
        try { brokenClient.GetStatesAsync().GetAwaiter().GetResult(); }
        catch (KuroProgressProtocolException) { statesRejected = true; }
        check(statesRejected, "an incomplete region list is rejected instead of treated as empty");

        // The upload direction is only valid if the request keeps matching the
        // contract verified against the live endpoint on 2026-09-17.
        var writeHandler = new KuroWriteHandler(new HttpResponseMessage(System.Net.HttpStatusCode.OK)
        {
            Content = new StringContent("{\"code\":200,\"data\":true}")
        });
        using var writeClient = new KuroMapProgressClient(new HttpClient(writeHandler));
        writeClient.SetCompletionAsync("secret-token", "device-test", 8, "1523072607719899136", "cx_01", true).GetAwaiter().GetResult();
        var write = writeHandler.Request!;
        check(write.Method == HttpMethod.Post && write.RequestUri!.AbsolutePath == "/map/core/position/changeStatus" &&
            write.Headers.GetValues("state_id").Single() == "8" && write.Content.Headers.ContentType!.MediaType == "application/json" &&
            writeHandler.Body.Contains("\"id\":\"1523072607719899136\"") && writeHandler.Body.Contains("\"positionType\":\"cx_01\"") &&
            writeHandler.Body.Contains("\"status\":1"),
            "completion upload uses the verified changeStatus contract");
        var rejectedWrite = new KuroWriteHandler(new HttpResponseMessage(System.Net.HttpStatusCode.OK)
        {
            Content = new StringContent("{\"code\":102,\"msg\":\"positionType 标点类型不能为空\"}")
        });
        using var rejectedClient = new KuroMapProgressClient(new HttpClient(rejectedWrite));
        bool writeRejected = false;
        try { rejectedClient.SetCompletionAsync("secret-token", "device-test", 8, "1527032607719899136", "cx_01", true).GetAwaiter().GetResult(); }
        catch (KuroProgressProtocolException) { writeRejected = true; }
        check(writeRejected, "a rejected completion upload is reported instead of silently ignored");

        // The desktop registers exactly the extension id Edge assigns, so the
        // constant must keep matching the public key embedded in the extension.
        string manifestPath = Path.Combine("BrowserExtensions", "KuroMapSync", "manifest.json");
        check(File.Exists(manifestPath), "the browser extension manifest is part of the checkout");
        using (var manifest = JsonDocument.Parse(File.ReadAllText(manifestPath)))
        {
            string key = manifest.RootElement.TryGetProperty("key", out var keyValue) ? keyValue.GetString() ?? "" : "";
            check(key.Length > 0 && KuroNativeBridgeHost.DeriveExtensionId(key) == KuroNativeBridgeHost.ExtensionId,
                "the registered extension id matches the public key embedded in the extension manifest");
            check(manifest.RootElement.GetProperty("permissions").EnumerateArray().Select(entry => entry.GetString()).ToHashSet().SetEquals(["nativeMessaging", "storage"]),
                "the extension requests only the permissions the bridge needs");
        }
        check(KuroNativeBridgeHost.HostManifestJson(@"C:\x\KuroSyncBridge.exe").Contains("\"allowed_origins\"") &&
            KuroNativeBridgeHost.SettingsJson().Contains(KuroNativeBridgeHost.Origin),
            "the host manifest and bridge settings carry the extension origin");

        // Automatic sync must fire soon after start-up, then settle on the normal
        // interval, and it must not hammer a failing endpoint.
        var schedule = new KuroSyncSchedule();
        check(schedule.NextDelay == KuroSyncSchedule.StartupDelay, "a fresh session waits the short start-up delay");
        schedule.RecordSuccess();
        check(schedule.NextDelay == KuroSyncSchedule.Interval && schedule.FailureCount == 0,
            "a successful pass settles on the normal interval");
        schedule.RecordManual();
        check(schedule.NextDelay == KuroSyncSchedule.Interval, "a manual sync restarts the normal interval");
        var ladder = new List<double>();
        for (int i = 0; i < 8; ++i) { schedule.RecordFailure(); ladder.Add(schedule.NextDelay.TotalSeconds); }
        check(ladder.SequenceEqual(new[] { 60d, 120d, 240d, 480d, 960d, 1800d, 1800d, 1800d }),
            "repeated failures back off exponentially and stop at the retry ceiling");
        schedule.RecordSuccess();
        check(schedule.NextDelay == KuroSyncSchedule.Interval && schedule.FailureCount == 0,
            "a recovered pass clears the failure backoff");
        schedule.RecordStartup();
        check(schedule.NextDelay == KuroSyncSchedule.StartupDelay, "switching the feature on arms the start-up delay");
        check(KuroSyncSchedule.MaximumRetry <= TimeSpan.FromMinutes(30) && KuroSyncSchedule.Interval >= TimeSpan.FromMinutes(5),
            "the automatic interval stays inside the documented bounds");
    }
}

internal sealed class KuroHttpHandler(HttpResponseMessage response) : HttpMessageHandler
{
    public HttpRequestMessage? Request { get; private set; }
    protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
    {
        Request = request;
        return Task.FromResult(response);
    }
}

/// <summary>Captures the request body while the content is still alive.</summary>
internal sealed class KuroWriteHandler(HttpResponseMessage response) : HttpMessageHandler
{
    public HttpRequestMessage? Request { get; private set; }
    public string Body { get; private set; } = "";
    protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
    {
        Request = request;
        Body = request.Content is null ? "" : await request.Content.ReadAsStringAsync(cancellationToken);
        return response;
    }
}
