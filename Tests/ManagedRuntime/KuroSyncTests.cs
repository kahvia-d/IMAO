using IMao_WinUI.Core.KuroSync;

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
