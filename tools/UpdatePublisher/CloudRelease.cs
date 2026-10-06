using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using IMao_WinUI.Core.Updates;

static partial class Publisher
{
    const int RequestLimit = 16 * 1024 * 1024;
    static readonly string[] RequestFiles = ["request.json", "catalog-payload.json", "approval-payload.json", "asset-inventory.json", "preparation-report.json", "previous-stable.json"];
    sealed record Approval(string Purpose, string TransactionId, string SourceCommit, string Version, string Tag,
        long Sequence, string BaselineId, long BuildRunId, long ArtifactId, string ArtifactDigest,
        string CatalogSha256, string InventorySha256, string ReportSha256, string PreviousStableSha256,
        bool OfflineNeeded);
    sealed record RequestIndex(int FormatVersion, string RequestId, string CatalogSha256, string ApprovalSha256);
    sealed record SigningResponse(int FormatVersion, string RequestId, string KeyId, string CatalogSha256,
        string ApprovalSha256, string CatalogSignature, string ApprovalSignature);
    sealed record ReleaseAsset(string Name, string? Path, long Size, string Sha256, string Url);
    static string BytesHash(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
    static void RequireHash(string value) { if (!Regex.IsMatch(value, "^[a-f0-9]{64}$")) throw new InvalidDataException("Invalid SHA-256."); }
    static string RequestId(string catalog, string approval) => BytesHash(Encoding.UTF8.GetBytes("imao-signing-request-v1\n" + catalog + "\n" + approval));
    static void RequireOwnedUrl(string value)
    {
        RequireGithub(value, value.Contains("/download/", StringComparison.Ordinal));
        var path = new Uri(value).AbsolutePath;
        if (!path.StartsWith("/" + RepoSlug + "/releases/", StringComparison.Ordinal) &&
            !path.StartsWith("/kahvia-d/WWMAP-TOOLS/releases/", StringComparison.Ordinal))
            throw new InvalidDataException("Signing request names an unrelated repository.");
    }
    static void CreateSigningRequest(Dictionary<string, string> o)
    {
        var input = Path.GetFullPath(Required(o, "input"));
        var output = Path.GetFullPath(Required(o, "output"));
        if (Directory.Exists(output)) throw new IOException("Request destination must be new.");
        var reportFile = Path.Combine(input, "release-report.json");
        var report = JsonNode.Parse(File.ReadAllBytes(reportFile))!.AsObject();
        var catalog = Read<UpdateCatalog>(Path.Combine(input, "catalog-draft.json"));
        var previous = Required(o, "previous");
        var keys = Read<TrustedUpdateKeys>(Required(o, "public-key"));
        var production = o.GetValueOrDefault("test") != "true";
        var prior = VerifyEnvelope(previous, keys, production);
        if (report["previousStableSha256"]?.GetValue<string>() != Hash(previous)) throw new InvalidDataException("Build's previous stable has changed; rebuild before requesting signatures.");
        var sequence = long.Parse(Required(o, "sequence"));
        if (sequence <= prior.Sequence) throw new InvalidDataException("Reserved sequence must advance stable.");
        var snapshotId = report["snapshotId"]!.GetValue<string>();
        catalog = catalog with { Sequence = sequence, Resources = catalog.Resources.Select(r => r.SnapshotId == snapshotId ? r with { Sequence = sequence } : r).ToList() };
        ValidateCatalog(catalog);
        var inventory = new List<ReleaseAsset>();
        foreach (var dir in new[] { "packages", "program", "manual" })
            if (Directory.Exists(Path.Combine(input, dir)))
                foreach (var file in Directory.GetFiles(Path.Combine(input, dir), "*", SearchOption.TopDirectoryOnly).Order(StringComparer.Ordinal))
                {
                    var relative = Relative(input, file); SafeFile(input, relative); UpdateStorage.RejectLink(file);
                    inventory.Add(new(Path.GetFileName(file), relative, new FileInfo(file).Length, Hash(file),
                        $"https://github.com/{RepoSlug}/releases/download/{report["tag"]!.GetValue<string>()}/{Uri.EscapeDataString(Path.GetFileName(file))}"));
                }
        // Every catalog URL, including retained assets, is audited locally and verified again before publishing.
        foreach (var asset in CatalogAssets(catalog))
        {
            var local = inventory.SingleOrDefault(a => a.Name == asset.Name && a.Sha256 == asset.Sha256);
            if (local is not null) inventory[inventory.IndexOf(local)] = local with { Url = asset.Url };
            else if (!inventory.Any(a => a.Url == asset.Url)) inventory.Add(asset);
        }
        if (inventory.GroupBy(a => a.Name, StringComparer.OrdinalIgnoreCase).Any(g => g.Where(a => a.Path != null).Count() > 1)) throw new InvalidDataException("Duplicate local asset name.");
        Directory.CreateDirectory(output);
        WriteNew(Path.Combine(output, "catalog-payload.json"), catalog);
        WriteNew(Path.Combine(output, "asset-inventory.json"), inventory);
        File.Copy(reportFile, Path.Combine(output, "preparation-report.json"));
        File.Copy(previous, Path.Combine(output, "previous-stable.json"));
        var approval = new Approval("imao-release-approval-v1", Id(Required(o, "transaction-id")),
            report["sourceCommit"]!.GetValue<string>(), report["appVersion"]!.GetValue<string>(), report["tag"]!.GetValue<string>(),
            sequence, report["baselineId"]!.GetValue<string>(), long.Parse(Required(o, "build-run-id")), long.Parse(Required(o, "artifact-id")),
            Required(o, "artifact-digest").Replace("sha256:", "", StringComparison.Ordinal), Hash(Path.Combine(output, "catalog-payload.json")),
            Hash(Path.Combine(output, "asset-inventory.json")), Hash(reportFile), Hash(previous), report["offlineNeeded"]!.GetValue<bool>());
        WriteNew(Path.Combine(output, "approval-payload.json"), approval);
        var approvalHash = Hash(Path.Combine(output, "approval-payload.json"));
        WriteNew(Path.Combine(output, "request.json"), new RequestIndex(1, RequestId(approval.CatalogSha256, approvalHash), approval.CatalogSha256, approvalHash));
        var validated = ValidateRequest(output, keys, production, approval.SourceCommit, approval.Version);
        CheckBulk(input, validated);
        Console.WriteLine("Created bounded signing request; no private key was loaded.");
    }
    static IEnumerable<ReleaseAsset> CatalogAssets(UpdateCatalog c)
    {
        foreach (var p in c.Resources.SelectMany(r => r.Packages)) yield return new(Path.GetFileName(new Uri(p.Url).AbsolutePath), null, p.Size, p.Sha256, p.Url);
        if (c.App.Package is not { } program) yield break;
        yield return new(Path.GetFileName(new Uri(program.Url).AbsolutePath), null, program.Size, program.Sha256, program.Url);
        foreach (var p in program.Shards) yield return new(Path.GetFileName(new Uri(p.Url).AbsolutePath), null, p.Size, p.Sha256, p.Url);
    }
    static (Approval Approval, RequestIndex Index, UpdateCatalog Catalog, List<ReleaseAsset> Assets) ValidateRequest(
        string root, TrustedUpdateKeys keys, bool production, string expectedSource, string expectedVersion)
    {
        long size = 0;
        var found = Directory.GetFiles(root, "*", SearchOption.AllDirectories);
        if (found.Length != RequestFiles.Length || found.Any(f => !RequestFiles.Contains(Relative(root, f), StringComparer.Ordinal))) throw new InvalidDataException("Unexpected signing-request files.");
        foreach (var file in found) { UpdateStorage.RejectLink(file); size += new FileInfo(file).Length; }
        if (size > RequestLimit) throw new InvalidDataException("Signing request exceeds 16 MiB.");
        var index = Read<RequestIndex>(Path.Combine(root, "request.json"));
        var a = Read<Approval>(Path.Combine(root, "approval-payload.json"));
        foreach (var hash in new[] { a.CatalogSha256, a.InventorySha256, a.ReportSha256, a.PreviousStableSha256, a.ArtifactDigest, index.ApprovalSha256 }) RequireHash(hash);
        if (index.FormatVersion != 1 || index.RequestId != RequestId(a.CatalogSha256, index.ApprovalSha256) || index.CatalogSha256 != a.CatalogSha256 ||
            Hash(Path.Combine(root, "approval-payload.json")) != index.ApprovalSha256 || Hash(Path.Combine(root, "catalog-payload.json")) != a.CatalogSha256 ||
            Hash(Path.Combine(root, "asset-inventory.json")) != a.InventorySha256 || Hash(Path.Combine(root, "preparation-report.json")) != a.ReportSha256 ||
            Hash(Path.Combine(root, "previous-stable.json")) != a.PreviousStableSha256) throw new CryptographicException("Signing request hashes disagree.");
        if (a.Purpose != "imao-release-approval-v1" || a.SourceCommit != expectedSource || !Regex.IsMatch(expectedSource, "^[a-f0-9]{40}$") || a.Version != expectedVersion || a.ArtifactId < 1 || a.BuildRunId < 1) throw new InvalidDataException("Signing request purpose, source, version or artifact identity differs.");
        Id(a.TransactionId); Id(a.Tag); FourPartVersion(a.Version);
        var c = Read<UpdateCatalog>(Path.Combine(root, "catalog-payload.json")); ValidateCatalog(c);
        var previous = VerifyEnvelope(Path.Combine(root, "previous-stable.json"), keys, production);
        var report = JsonNode.Parse(File.ReadAllBytes(Path.Combine(root, "preparation-report.json")))!;
        if (a.Sequence <= previous.Sequence || c.Sequence != a.Sequence || c.App.Version != a.Version || c.App.Package?.SourceCommit != a.SourceCommit ||
            report["appVersion"]!.GetValue<string>() != a.Version || report["sourceCommit"]!.GetValue<string>() != a.SourceCommit ||
            report["baselineId"]!.GetValue<string>() != a.BaselineId || report["tag"]!.GetValue<string>() != a.Tag ||
            report["previousStableSha256"]!.GetValue<string>() != a.PreviousStableSha256 || report["offlineNeeded"]!.GetValue<bool>() != a.OfflineNeeded ||
            c.Resources.Single(r => r.SnapshotId == report["snapshotId"]!.GetValue<string>()).Sequence != a.Sequence)
            throw new InvalidDataException("Catalog, build report and approval disagree.");
        if (production && (report["production"]?.GetValue<bool>() != true || report["sourceDirty"]?.GetValue<bool>() != false || report["nativePassed"]?.GetValue<bool>() != true ||
            !Regex.IsMatch(report["sourceTreeSha256"]?.GetValue<string>() ?? "", "^[a-f0-9]{64}$"))) throw new InvalidDataException("Production signing requires a clean, native-verified build report.");
        RequireOwnedUrl(c.App.Url);
        if (c.App.Url != $"https://github.com/{RepoSlug}/releases/tag/{a.Tag}") throw new InvalidDataException("Release URL/tag differs.");
        var assets = Read<List<ReleaseAsset>>(Path.Combine(root, "asset-inventory.json"));
        foreach (var asset in assets) { RequireOwnedUrl(asset.Url); RequireHash(asset.Sha256); if (asset.Size <= 0 || asset.Size >= 2L * 1024 * 1024 * 1024 || asset.Name != Path.GetFileName(new Uri(asset.Url).AbsolutePath)) throw new InvalidDataException("Invalid asset inventory."); if (asset.Path != null) SafeFile(root, asset.Path); }
        foreach (var asset in CatalogAssets(c))
            if (!assets.Any(x => x.Url == asset.Url && x.Size == asset.Size && x.Sha256 == asset.Sha256)) throw new InvalidDataException("Catalog asset is missing from inventory.");
        if (assets.Count(x => x.Path?.StartsWith("manual/", StringComparison.Ordinal) == true && x.Name == $"IMao-v{a.Version}-windows-x64.zip") != 1) throw new InvalidDataException("Approval must bind exactly one matching first-install ZIP.");
        return (a, index, c, assets);
    }
    static void SignRequest(Dictionary<string, string> o)
    {
        if (!OperatingSystem.IsWindows() || (Environment.GetEnvironmentVariable("GITHUB_ACTIONS") == "true" && o.GetValueOrDefault("test") != "true")) throw new InvalidOperationException("Production signing is local Windows only.");
        var root = Required(o, "input"); var production = o.GetValueOrDefault("test") != "true";
        var keys = Read<TrustedUpdateKeys>(Required(o, "public-key"));
        var r = ValidateRequest(root, keys, production, Required(o, "expected-source-commit"), Required(o, "confirm-version"));
        using var key = LoadPrivate(Required(o, "private-key"), production, out var id);
        var trusted = keys.Keys.Single(k => k.KeyId == id && (!production || !k.TestOnly));
        if (!CryptographicOperations.FixedTimeEquals(key.ExportSubjectPublicKeyInfo(), Convert.FromBase64String(trusted.PublicKey))) throw new CryptographicException("Local key differs from trusted registry.");
        string Signature(string name) => Convert.ToBase64String(key.SignData(File.ReadAllBytes(Path.Combine(root, name)), HashAlgorithmName.SHA256, DSASignatureFormat.IeeeP1363FixedFieldConcatenation));
        WriteNew(Required(o, "output"), new SigningResponse(1, r.Index.RequestId, id, r.Index.CatalogSha256, r.Index.ApprovalSha256, Signature("catalog-payload.json"), Signature("approval-payload.json")));
        Console.WriteLine($"Signed {r.Approval.Version}, source {r.Approval.SourceCommit}, sequence {r.Approval.Sequence}, request {r.Index.RequestId}. Private material stayed in memory.");
    }
    static (Approval Approval, RequestIndex Index, UpdateCatalog Catalog, List<ReleaseAsset> Assets) VerifyResponse(Dictionary<string, string> o)
    {
        var root = Required(o, "request"); var keys = Read<TrustedUpdateKeys>(Required(o, "public-key"));
        var r = ValidateRequest(root, keys, o.GetValueOrDefault("test") != "true", Required(o, "expected-source-commit"), Required(o, "confirm-version"));
        var responseFile = Required(o, "response"); if (new FileInfo(responseFile).Length > 16384) throw new InvalidDataException("Signature response exceeds 16 KiB.");
        var response = Read<SigningResponse>(responseFile);
        if (response.FormatVersion != 1 || response.RequestId != r.Index.RequestId || response.CatalogSha256 != r.Index.CatalogSha256 || response.ApprovalSha256 != r.Index.ApprovalSha256) throw new CryptographicException("Response does not bind this request.");
        var trusted = keys.Keys.Single(k => k.KeyId == response.KeyId && (o.GetValueOrDefault("test") == "true" || !k.TestOnly));
        using var key = ECDsa.Create(); var spki = Convert.FromBase64String(trusted.PublicKey); key.ImportSubjectPublicKeyInfo(spki, out var read);
        if (read != spki.Length || key.KeySize != 256) throw new CryptographicException("P-256 public key required.");
        foreach (var pair in new[] { ("catalog-payload.json", response.CatalogSignature), ("approval-payload.json", response.ApprovalSignature) })
        { var sig = Convert.FromBase64String(pair.Item2); if (sig.Length != 64 || !key.VerifyData(File.ReadAllBytes(Path.Combine(root, pair.Item1)), sig, HashAlgorithmName.SHA256, DSASignatureFormat.IeeeP1363FixedFieldConcatenation)) throw new CryptographicException("Invalid detached signature."); }
        return r;
    }
    static void CheckBulk(string root, (Approval Approval, RequestIndex Index, UpdateCatalog Catalog, List<ReleaseAsset> Assets) r)
    {
        foreach (var a in r.Assets.Where(a => a.Path != null)) {
            var file = SafeFile(root, a.Path!); UpdateStorage.RejectLink(file);
            if (new FileInfo(file).Length != a.Size || Hash(file) != a.Sha256) throw new InvalidDataException($"Frozen build asset changed: {a.Name}");
        }
        var actual = new[] { "packages", "program", "manual" }.Where(d => Directory.Exists(Path.Combine(root, d))).SelectMany(d => Directory.GetFiles(Path.Combine(root, d), "*", SearchOption.AllDirectories)).Select(f => Relative(root, f)).ToHashSet(StringComparer.Ordinal);
        if (!actual.SetEquals(r.Assets.Where(a => a.Path != null).Select(a => a.Path!))) throw new InvalidDataException("Frozen build contains unexpected or missing assets.");
        var manual = r.Assets.Single(a => a.Path?.StartsWith("manual/", StringComparison.Ordinal) == true && a.Name == $"IMao-v{r.Approval.Version}-windows-x64.zip");
        // Checking ZIP entries against the signed shard inventory binds installation and updater to one tree.
        VerifyPackage(SafeFile(root, manual.Path!), new ResourcePackage { Id = "manual", Size = manual.Size, Sha256 = manual.Sha256, Files = r.Catalog.App.Package!.Files });
    }
    static async Task FinalizeRelease(Dictionary<string, string> o)
    {
        var r = VerifyResponse(o); var root = Required(o, "input"); CheckBulk(root, r);
        var response = Read<SigningResponse>(Required(o, "response"));
        var signed = new SignedUpdateEnvelope { KeyId = response.KeyId, Payload = Convert.ToBase64String(File.ReadAllBytes(Path.Combine(Required(o, "request"), "catalog-payload.json"))), Signature = response.CatalogSignature };
        var manifest = Path.Combine(root, "update.json");
        var signedBytes = JsonSerializer.SerializeToUtf8Bytes(signed, Json);
        if (File.Exists(manifest)) { if (!File.ReadAllBytes(manifest).AsSpan().SequenceEqual(signedBytes)) throw new CryptographicException("Already finalized with a different signature; use the original response."); }
        else File.WriteAllBytes(manifest, signedBytes);
        VerifyEnvelope(manifest, Read<TrustedUpdateKeys>(Required(o, "public-key")), o.GetValueOrDefault("test") != "true");
        var report = JsonNode.Parse(File.ReadAllBytes(Path.Combine(Required(o, "request"), "preparation-report.json")))!.AsObject();
        report["sequence"] = r.Approval.Sequence; report["signedManifestSha256"] = Hash(manifest); report["requestId"] = r.Index.RequestId;
        if (r.Approval.OfflineNeeded)
        {
            var name = $"resources-{r.Approval.Version}-offline.zip"; var file = Path.Combine(root, name);
            var temp = file + ".new"; if (File.Exists(temp)) File.Delete(temp);
            using (var zip = new ZipArchive(new FileStream(temp, FileMode.CreateNew), ZipArchiveMode.Create)) {
                AddFile(zip, manifest, "update.json", CompressionLevel.Optimal);
                var snapshot = r.Catalog.Resources.Single(x => x.SnapshotId == report["snapshotId"]!.GetValue<string>());
                foreach (var p in snapshot.Packages.OrderBy(p => p.Id, StringComparer.Ordinal)) { var n = $"{p.Id}-{p.Version}.zip"; AddFile(zip, Path.Combine(root, "packages", n), "packages/" + n, CompressionLevel.NoCompression); }
            }
            if (new FileInfo(temp).Length >= 2L * 1024 * 1024 * 1024) throw new InvalidDataException("Offline ZIP exceeds GitHub's attachment limit.");
            if (File.Exists(file) && Hash(file) != Hash(temp)) throw new InvalidDataException("Existing offline ZIP differs.");
            File.Move(temp, file, true); report["offline"] = JsonSerializer.SerializeToNode(new { name, size = new FileInfo(file).Length, sha256 = Hash(file) }, Json);
        }
        File.WriteAllBytes(Path.Combine(root, "release-report.json"), JsonSerializer.SerializeToUtf8Bytes(report, Json));
        var requestArchive = Path.Combine(root, "release-signing-request.zip");
        var requestTemp = requestArchive + ".new"; if (File.Exists(requestTemp)) File.Delete(requestTemp);
        using (var zip = new ZipArchive(new FileStream(requestTemp, FileMode.CreateNew), ZipArchiveMode.Create))
            foreach (var name in RequestFiles.Order(StringComparer.Ordinal)) AddFile(zip, Path.Combine(Required(o, "request"), name), name, CompressionLevel.Optimal);
        if (File.Exists(requestArchive) && Hash(requestArchive) != Hash(requestTemp)) throw new InvalidDataException("Original audit request archive differs.");
        File.Move(requestTemp, requestArchive, true);
        var auth = new { approval = r.Approval, inventoryPayload = Convert.ToBase64String(File.ReadAllBytes(Path.Combine(Required(o, "request"), "asset-inventory.json"))), approvalPayload = Convert.ToBase64String(File.ReadAllBytes(Path.Combine(Required(o, "request"), "approval-payload.json"))), response };
        File.WriteAllBytes(Path.Combine(root, "release-authorization.json"), JsonSerializer.SerializeToUtf8Bytes(auth, Json));
        Verify(new() { ["input"] = root, ["public-key"] = Required(o, "public-key"), ["snapshot-id"] = report["snapshotId"]!.GetValue<string>(), ["test"] = o.GetValueOrDefault("test", "false") });
        await Task.CompletedTask;
        Console.WriteLine("Finalized verified detached signatures and frozen build; no private key was loaded.");
    }
    static void VerifyReleaseAuthorization(Dictionary<string, string> o)
    {
        var r = VerifyResponse(o); CheckBulk(Required(o, "input"), r);
        var envelope = Read<SignedUpdateEnvelope>(Path.Combine(Required(o, "input"), "update.json"));
        var response = Read<SigningResponse>(Required(o, "response"));
        if (envelope.Signature != response.CatalogSignature || envelope.KeyId != response.KeyId || BytesHash(Convert.FromBase64String(envelope.Payload)) != r.Index.CatalogSha256) throw new CryptographicException("Final manifest differs from authorized response.");
        FinalizeRelease(o).GetAwaiter().GetResult();
        Console.WriteLine("Release authorization verified.");
    }
    static void VerifyInstallAuthorization(Dictionary<string, string> o)
    {
        var file = Required(o, "input");
        if (new FileInfo(file).Length > RequestLimit) throw new InvalidDataException("Authorization attachment exceeds size limit.");
        var doc = Read<JsonElement>(file);
        var payload = Convert.FromBase64String(doc.GetProperty("approvalPayload").GetString()!);
        var inventory = Convert.FromBase64String(doc.GetProperty("inventoryPayload").GetString()!);
        var a = JsonSerializer.Deserialize<Approval>(payload, Json)!;
        var response = doc.GetProperty("response").Deserialize<SigningResponse>(Json)!;
        var keys = Read<TrustedUpdateKeys>(Required(o, "public-key"));
        var trusted = keys.Keys.Single(k => k.KeyId == response.KeyId && (o.GetValueOrDefault("test") == "true" || !k.TestOnly));
        using var key = ECDsa.Create(); key.ImportSubjectPublicKeyInfo(Convert.FromBase64String(trusted.PublicKey), out _);
        var sig = Convert.FromBase64String(response.ApprovalSignature);
        if (a.Purpose != "imao-release-approval-v1" || sig.Length != 64 || !key.VerifyData(payload, sig, HashAlgorithmName.SHA256, DSASignatureFormat.IeeeP1363FixedFieldConcatenation) ||
            a.InventorySha256 != BytesHash(inventory) || response.ApprovalSha256 != BytesHash(payload) || response.CatalogSha256 != a.CatalogSha256 || response.RequestId != RequestId(a.CatalogSha256, BytesHash(payload))) throw new CryptographicException("Invalid installation authorization.");
        var manifest = Required(o, "manifest"); var c = VerifyEnvelope(manifest, keys, o.GetValueOrDefault("test") != "true");
        var envelope = Read<SignedUpdateEnvelope>(manifest);
        if (c.Sequence != a.Sequence || c.App.Version != a.Version || c.App.Package?.SourceCommit != a.SourceCommit ||
            BytesHash(Convert.FromBase64String(envelope.Payload)) != a.CatalogSha256 || envelope.Signature != response.CatalogSignature || envelope.KeyId != response.KeyId ||
            c.App.Url != $"https://github.com/{RepoSlug}/releases/tag/{a.Tag}") throw new CryptographicException("Installation authorization differs from signed stable.");
        var assets = JsonSerializer.Deserialize<List<ReleaseAsset>>(inventory, Json)!;
        var archive = Required(o, "program-zip");
        var authorized = assets.Single(x => x.Name == Path.GetFileName(archive) && x.Path == "manual/" + Path.GetFileName(archive));
        if (authorized.Name != $"IMao-v{a.Version}-windows-x64.zip" || Hash(archive) != authorized.Sha256 || new FileInfo(archive).Length != authorized.Size) throw new CryptographicException("Installation archive differs from signed approval.");
        VerifyPackage(archive, new ResourcePackage { Id = "manual", Size = authorized.Size, Sha256 = authorized.Sha256, Files = c.App.Package!.Files });
        Console.WriteLine("Installation ZIP matches production approval and the client's signed program tree.");
    }
}
