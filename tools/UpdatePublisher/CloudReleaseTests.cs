using System.IO.Compression;
using System.Text.Json;
using System.Text.Json.Nodes;
using IMao_WinUI.Core.Updates;

static partial class Publisher
{
    static async Task CloudSelfTest(string root)
    {
        root = Path.GetFullPath(root); Directory.CreateDirectory(root);
        var fixtures = Path.Combine(root, "fixtures"); SelfTest(fixtures);
        var app = Path.Combine(fixtures, "shard-release-app");
        var buildPath = Path.Combine(app, "build-info.json");
        var build = JsonNode.Parse(File.ReadAllBytes(buildPath))!; build["appVersion"] = "2026.9.9.8";
        File.WriteAllBytes(buildPath, JsonSerializer.SerializeToUtf8Bytes(build, Json));
        var previous = Path.Combine(fixtures, "shard-release-3", "update.json");
        var publicKey = Path.Combine(fixtures, "fixture-public.json");
        var bulk = Path.Combine(root, "bulk");
        var o = new Dictionary<string, string> { ["app-root"] = app, ["public-key"] = publicKey, ["output"] = bulk, ["previous"] = previous,
            ["sequence"] = "8", ["resource-version"] = "2026.9.9.8", ["tag"] = "v2026.9.9.8", ["test"] = "true", ["unsigned"] = "true", ["program-release"] = "true", ["program-shards"] = "true" };
        await Prepare(o);
        if (File.Exists(Path.Combine(bulk, "update.json"))) throw new Exception("Unsigned build emitted a signed manifest.");
        Directory.CreateDirectory(Path.Combine(bulk, "manual"));
        ZipFile.CreateFromDirectory(app, Path.Combine(bulk, "manual", "IMao-v2026.9.9.8-windows-x64.zip"));
        var request = Path.Combine(root, "request");
        var create = new Dictionary<string, string> { ["input"] = bulk, ["output"] = request, ["previous"] = previous, ["public-key"] = publicKey, ["test"] = "true",
            ["sequence"] = "9", ["transaction-id"] = "fixture-txn", ["build-run-id"] = "123", ["artifact-id"] = "456", ["artifact-digest"] = new string('c', 64) };
        CreateSigningRequest(create);
        var response = Path.Combine(root, "response.json");
        var sign = new Dictionary<string, string> { ["input"] = request, ["output"] = response, ["public-key"] = publicKey, ["test"] = "true",
            ["private-key"] = Path.Combine(fixtures, "fixture-private.json"), ["expected-source-commit"] = new string('a', 40), ["confirm-version"] = "2026.9.9.8" };
        var passed = new List<string>();
        void Reject(string name, Action action) { try { action(); } catch { passed.Add(name); return; } throw new Exception("Expected rejection: " + name); }
        // A release run builds a production-labelled tree and rehearses that exact frozen tree with a
        // disposable key, so the disposable path must accept a report whose production flag is set.
        var labelled = Path.Combine(root, "production-labelled");
        foreach (var file in Directory.GetFiles(bulk, "*", SearchOption.AllDirectories))
        {
            var target = Path.Combine(labelled, Path.GetRelativePath(bulk, file));
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            File.Copy(file, target);
        }
        var labelledReport = Path.Combine(labelled, "release-report.json");
        var labelledJson = JsonNode.Parse(File.ReadAllBytes(labelledReport))!; labelledJson["production"] = true;
        File.WriteAllBytes(labelledReport, JsonSerializer.SerializeToUtf8Bytes(labelledJson, Json));
        var labelledRequest = Path.Combine(root, "labelled-request"); var labelledResponse = Path.Combine(root, "labelled-response.json");
        CreateSigningRequest(new(create) { ["input"] = labelled, ["output"] = labelledRequest, ["transaction-id"] = "labelled-txn" });
        var labelledSign = new Dictionary<string, string>(sign) { ["input"] = labelledRequest, ["output"] = labelledResponse };
        SignRequest(labelledSign);
        await FinalizeRelease(new(labelledSign) { ["input"] = labelled, ["request"] = labelledRequest, ["response"] = labelledResponse });
        passed.Add("disposable-key rehearsal accepts a production-labelled frozen tree");
        sign["confirm-version"] = "2026.9.9.9"; Reject("wrong confirmed version", () => SignRequest(sign)); sign["confirm-version"] = "2026.9.9.8";
        sign["expected-source-commit"] = new string('b', 40); Reject("wrong expected source", () => SignRequest(sign)); sign["expected-source-commit"] = new string('a', 40);
        var catalogFile = Path.Combine(request, "catalog-payload.json"); var catalogBytes = File.ReadAllBytes(catalogFile);
        File.AppendAllText(catalogFile, " "); Reject("payload bytes replaced", () => SignRequest(sign)); File.WriteAllBytes(catalogFile, catalogBytes);
        var previousActions = Environment.GetEnvironmentVariable("GITHUB_ACTIONS"); Environment.SetEnvironmentVariable("GITHUB_ACTIONS", "true");
        sign["test"] = "false"; Reject("signing disabled inside Actions", () => SignRequest(sign)); sign["test"] = "true";
        // A fixture relabelled as non-test must be rejected before DPAPI, even with --test true.
        var nonTestFile = Path.Combine(root, "non-test-labelled-fixture.json");
        var nonTest = JsonNode.Parse(File.ReadAllBytes(sign["private-key"]))!; nonTest["testOnly"] = false;
        File.WriteAllBytes(nonTestFile, JsonSerializer.SerializeToUtf8Bytes(nonTest, Json));
        Reject("production-labelled key cannot bypass Actions through --test", () => SignRequest(new(sign) { ["private-key"] = nonTestFile, ["output"] = Path.Combine(root, "forbidden-response.json") }));
        Environment.SetEnvironmentVariable("GITHUB_ACTIONS", previousActions);
        SignRequest(sign);
        var finish = new Dictionary<string, string>(sign) { ["input"] = bulk, ["request"] = request, ["response"] = response }; finish.Remove("private-key"); finish.Remove("output");
        var manual = Directory.GetFiles(Path.Combine(bulk, "manual"))[0]; var manualBytes = File.ReadAllBytes(manual);
        File.AppendAllText(manual, "changed"); Reject("signed manual ZIP replaced", () => FinalizeRelease(finish).GetAwaiter().GetResult()); File.WriteAllBytes(manual, manualBytes);
        var program = Directory.GetFiles(Path.Combine(bulk, "program"), "*.zip")[0]; var shardBytes = File.ReadAllBytes(program);
        File.AppendAllText(program, "changed"); Reject("signed shard replaced", () => FinalizeRelease(finish).GetAwaiter().GetResult()); File.WriteAllBytes(program, shardBytes);
        var responseBytes = File.ReadAllBytes(response); var altered = Read<SigningResponse>(response) with { ApprovalSignature = Read<SigningResponse>(response).CatalogSignature };
        File.WriteAllBytes(response, JsonSerializer.SerializeToUtf8Bytes(altered, Json)); Reject("approval/catalog signature substitution", () => FinalizeRelease(finish).GetAwaiter().GetResult()); File.WriteAllBytes(response, responseBytes);
        await FinalizeRelease(finish); VerifyReleaseAuthorization(finish);
        var manifestHash = Hash(Path.Combine(bulk, "update.json")); await FinalizeRelease(finish);
        if (manifestHash != Hash(Path.Combine(bulk, "update.json"))) throw new Exception("Resume changed signed bytes.");
        VerifyInstallAuthorization(new() { ["input"] = Path.Combine(bulk, "release-authorization.json"), ["program-zip"] = manual, ["manifest"] = Path.Combine(bulk, "update.json"), ["public-key"] = publicKey, ["test"] = "true" });
        File.AppendAllText(manual, "changed"); Reject("public installation authorization rejects swapped ZIP", () => VerifyInstallAuthorization(new() { ["input"] = Path.Combine(bulk, "release-authorization.json"), ["program-zip"] = manual, ["manifest"] = Path.Combine(bulk, "update.json"), ["public-key"] = publicKey, ["test"] = "true" })); File.WriteAllBytes(manual, manualBytes);
        var c = VerifyEnvelope(Path.Combine(bulk, "update.json"), Read<TrustedUpdateKeys>(publicKey), false);
        if (c.Sequence != 9) throw new Exception("Locked sequence wasn't finalized.");
        passed.Add("unsigned prepare / bounded request / DPAPI local sign / client verify / idempotent finalize");
        WriteNew(Path.Combine(root, "cloud-self-test-report.json"), new { passed = true, checks = passed });
        Console.WriteLine($"Cloud release tests passed ({passed.Count}); production key was never used.");
    }
}
