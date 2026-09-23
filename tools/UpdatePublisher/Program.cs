using System.Diagnostics;
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using IMao_WinUI.Core.Updates;

return await Publisher.Run(args);

static class Publisher
{
    static readonly JsonSerializerOptions Json = UpdateJson.Options;
    static readonly DateTimeOffset ZipEpoch = new(2020, 1, 1, 0, 0, 0, TimeSpan.Zero);
    static readonly HashSet<string> AllowedExtensions = new(StringComparer.OrdinalIgnoreCase)
        { ".json", ".png", ".jpg", ".jpeg", ".webp", ".imf", ".imx", ".yml", ".yaml", ".md", ".txt" };
    // The repository moved kahvia-d/WWMAP-TOOLS -> kahvia-d/IMAO. Newly published URLs use the
    // current slug; the legacy one is still accepted so an already-published catalog can be
    // verified or reused after the rename (GitHub redirects those downloads).
    const string RepoSlug = "kahvia-d/IMAO";
    static readonly string[] AcceptedRepoSlugs = { "kahvia-d/IMAO", "kahvia-d/WWMAP-TOOLS" };
    public static async Task<int> Run(string[] args)
    {
        try
        {
            if (args.Length == 0) throw new ArgumentException("Commands: init-key, prepare, verify, shard-map, self-test. See Docs/ResourceUpdates.md.");
            var options = Parse(args.Skip(1).ToArray());
            switch (args[0])
            {
                case "init-key": InitKey(options); break;
                case "prepare": await Prepare(options); break;
                case "verify": Verify(options); break;
                case "shard-map": ShardMapReport(options); break;
                case "verify-manifest":
                    var verified = VerifyEnvelope(Required(options, "input"), Read<TrustedUpdateKeys>(Required(options, "public-key")), options.GetValueOrDefault("test") != "true");
                    Console.WriteLine(JsonSerializer.Serialize(new { verified.Sequence, appVersion = verified.App.Version, snapshotIds = verified.Resources.Select(r => r.SnapshotId) }, Json));
                    break;
                case "self-test": SelfTest(Required(options, "output")); break;
                default: throw new ArgumentException("Unknown command.");
            }
            return 0;
        }
        catch (Exception ex) { Console.Error.WriteLine($"UpdatePublisher: {ex.Message}"); return 1; }
    }

    static Dictionary<string, string> Parse(string[] args)
    {
        var result = new Dictionary<string, string>(StringComparer.Ordinal);
        for (var i = 0; i < args.Length; i++)
        {
            if (!args[i].StartsWith("--") || i + 1 == args.Length || args[i + 1].StartsWith("--"))
                throw new ArgumentException("Options use --name value, including --test true.");
            if (!result.TryAdd(args[i][2..], args[++i])) throw new ArgumentException("Duplicate option.");
        }
        return result;
    }
    static string Required(Dictionary<string, string> o, string key) => o.TryGetValue(key, out var v) && v.Length > 0 ? v : throw new ArgumentException($"Missing --{key}.");
    static T Read<T>(string file) => JsonSerializer.Deserialize<T>(File.ReadAllBytes(file), Json) ?? throw new InvalidDataException($"Invalid JSON: {Path.GetFileName(file)}");
    static void WriteNew<T>(string file, T value)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(file))!);
        using var stream = new FileStream(file, FileMode.CreateNew);
        JsonSerializer.Serialize(stream, value, Json);
    }
    static string Hash(string file) { using var f = File.OpenRead(file); return Convert.ToHexString(SHA256.HashData(f)).ToLowerInvariant(); }
    static string Id(string id)
    {
        if (!Regex.IsMatch(id, "^[A-Za-z0-9][A-Za-z0-9._-]{0,100}$") || id.Contains("..")) throw new InvalidDataException("Invalid package/key identifier.");
        return id;
    }
    static string Relative(string root, string full) => Path.GetRelativePath(root, full).Replace('\\', '/');
    static string Absolute(string assets, string? relative) =>
        string.IsNullOrEmpty(relative) ? "" : Path.Combine(assets, relative);
    static string SafeFile(string root, string relative)
    {
        UpdateStorage.ValidateRelativePath(relative);
        if (string.IsNullOrWhiteSpace(relative) || relative.Contains('\\') || relative.Contains(':') || relative.StartsWith('/') ||
            relative.Split('/').Any(s => s is "." or ".." or "" || s.EndsWith('.') || s.EndsWith(' '))) throw new InvalidDataException("Unsafe archive path.");
        var full = Path.GetFullPath(Path.Combine(root, relative));
        if (!full.StartsWith(Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Archive path escapes its root.");
        return full;
    }
    static void InitKey(Dictionary<string, string> o)
    {
        var repo = Path.GetFullPath(Required(o, "repo"));
        var destination = Path.GetFullPath(Required(o, "private-key"));
        var test = o.GetValueOrDefault("test") == "true";
        if (!test && destination.StartsWith(repo.TrimEnd('\\', '/') + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("Production private keys must be outside the repository.");
        var publicFile = Required(o, "public-key");
        if (File.Exists(destination) || File.Exists(publicFile)) throw new IOException("Key destination already exists; refusing overwrite.");
        using var key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        var privateBytes = key.ExportPkcs8PrivateKey();
        try
        {
            var protectedBytes = Dpapi.Protect(privateBytes);
            WriteNew(destination, new PrivateKey(Id(Required(o, "key-id")), test, Convert.ToBase64String(protectedBytes)));
            WriteNew(publicFile, new TrustedUpdateKeys { Keys = [new() { KeyId = Required(o, "key-id"), TestOnly = test, PublicKey = Convert.ToBase64String(key.ExportSubjectPublicKeyInfo()) }] });
        }
        finally { CryptographicOperations.ZeroMemory(privateBytes); }
        Console.WriteLine("Created DPAPI CurrentUser signing key and public registry. Private material was not printed.");
    }
    static ECDsa LoadPrivate(string path, bool production, out string keyId)
    {
        var stored = Read<PrivateKey>(path);
        if (production && stored.TestOnly) throw new InvalidOperationException("Production preparation rejects a test signing key.");
        keyId = Id(stored.KeyId);
        var plain = Dpapi.Unprotect(Convert.FromBase64String(stored.ProtectedPkcs8));
        try { var key = ECDsa.Create(); key.ImportPkcs8PrivateKey(plain, out _); if (key.KeySize != 256) throw new CryptographicException("P-256 required."); return key; }
        finally { CryptographicOperations.ZeroMemory(plain); }
    }
    static SignedUpdateEnvelope Sign(UpdateCatalog catalog, ECDsa key, string id)
    {
        var payload = JsonSerializer.SerializeToUtf8Bytes(catalog, Json);
        return new() { KeyId = id, Payload = Convert.ToBase64String(payload), Signature = Convert.ToBase64String(key.SignData(payload, HashAlgorithmName.SHA256, DSASignatureFormat.IeeeP1363FixedFieldConcatenation)) };
    }
    static UpdateCatalog VerifyEnvelope(string file, TrustedUpdateKeys keys, bool production)
    {
        // Always use the client's authoritative verifier first, including its limits and path policy.
        UpdateSignature.Verify(File.ReadAllBytes(file), keys.Keys, allowTestKeys: !production);
        var envelope = Read<SignedUpdateEnvelope>(file);
        var trusted = keys.Keys.SingleOrDefault(k => k.KeyId == envelope.KeyId) ?? throw new CryptographicException("Unknown release signing key.");
        if (production && trusted.TestOnly) throw new CryptographicException("Production verification rejects test keys.");
        using var key = ECDsa.Create();
        key.ImportSubjectPublicKeyInfo(Convert.FromBase64String(trusted.PublicKey), out _);
        var bytes = Convert.FromBase64String(envelope.Payload);
        if (key.KeySize != 256 || !key.VerifyData(bytes, Convert.FromBase64String(envelope.Signature), HashAlgorithmName.SHA256, DSASignatureFormat.IeeeP1363FixedFieldConcatenation)) throw new CryptographicException("Invalid release signature.");
        var catalog = JsonSerializer.Deserialize<UpdateCatalog>(bytes, Json) ?? throw new InvalidDataException("Invalid release catalog.");
        ValidateCatalog(catalog);
        return catalog;
    }
    static void ValidateCatalog(UpdateCatalog c)
    {
        UpdateSignature.ValidateCatalog(c);
        if (c.SchemaVersion != 1 || c.Sequence < 1 || c.Resources.Count == 0) throw new InvalidDataException("Invalid catalog version, sequence, or empty resources.");
        FourPartVersion(c.App.Version);
        RequireGithub(c.App.Url, false);
        var identities = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var r in c.Resources)
        {
            Id(r.SnapshotId); Id(r.BaselineId);
            if (r.Sequence < 1 || r.Sequence > c.Sequence || r.Packages.Count(p => p.Kind == "map-data") != 1) throw new InvalidDataException("Each snapshot requires exactly one map-data package and valid sequence.");
            FourPartVersion(r.MinAppVersion);
            if (r.MaxAppVersion is not null && Version.Parse(r.MaxAppVersion) < Version.Parse(r.MinAppVersion)) throw new InvalidDataException("Invalid compatibility range.");
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var p in r.Packages)
            {
                Id(p.Id); FourPartVersion(p.Version);
                if (!seen.Add(p.Id) || p.Kind is not ("map-data" or "map-icons" or "tile" or "candidate") || p.Size <= 0 || !Regex.IsMatch(p.Sha256, "^[a-fA-F0-9]{64}$") || p.Files.Count == 0) throw new InvalidDataException("Invalid or duplicate package.");
                RequireGithub(p.Url, true);
                var identity = p.Id + "/" + p.Version;
                if (identities.TryGetValue(identity, out var hash) && hash != p.Sha256) throw new InvalidDataException("Same package version has different content.");
                identities[identity] = p.Sha256;
                var paths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                foreach (var f in p.Files)
                {
                    SafeFile(Path.GetTempPath(), f.Path);
                    if (!AllowedExtensions.Contains(Path.GetExtension(f.Path)) || !paths.Add(f.Path) || f.Size < 0 || !Regex.IsMatch(f.Sha256, "^[a-fA-F0-9]{64}$")) throw new InvalidDataException("Invalid package file list.");
                }
            }
        }
    }
    static void FourPartVersion(string value)
    {
        if (!Regex.IsMatch(value, "^[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+$") || !Version.TryParse(value, out var version) || version.Major > 65535 || version.Minor > 65535 || version.Build > 65535 || version.Revision > 65535)
            throw new InvalidDataException("Versions must contain four nonnegative 16-bit numeric components.");
    }
    static void RequireGithub(string value, bool asset)
    {
        if (!Uri.TryCreate(value, UriKind.Absolute, out var uri) || uri.Scheme != "https" || uri.Host != "github.com" ||
            !AcceptedRepoSlugs.Any(slug => uri.AbsolutePath.StartsWith(asset ? $"/{slug}/releases/download/" : $"/{slug}/releases/", StringComparison.Ordinal)) || uri.UserInfo.Length != 0 || uri.Query.Length != 0)
            throw new InvalidDataException("Only this repository's GitHub Releases URLs are permitted.");
    }
    static ResourcePackage Package(string source, string output, SnapshotPackage package, string baseUrl)
    {
        var files = Directory.GetFiles(source, "*", SearchOption.AllDirectories).OrderBy(p => Relative(source, p), StringComparer.Ordinal).ToArray();
        if (files.Length == 0) throw new InvalidDataException("Empty resource package.");
        var manifest = new List<ResourceFile>();
        foreach (var file in files)
        {
            UpdateStorage.RejectLink(file);
            if ((File.GetAttributes(file) & FileAttributes.ReparsePoint) != 0 || !AllowedExtensions.Contains(Path.GetExtension(file))) throw new InvalidDataException($"Unapproved resource file: {Relative(source, file)}");
            manifest.Add(new() { Path = Relative(source, file), Size = new FileInfo(file).Length, Sha256 = Hash(file) });
        }
        var name = $"{Id(package.Id)}-{Id(package.Version)}.zip";
        var destination = Path.Combine(output, "packages", name);
        Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
        using (var archive = new ZipArchive(new FileStream(destination, FileMode.CreateNew), ZipArchiveMode.Create))
        {
            foreach (var file in manifest)
            {
                var entry = archive.CreateEntry(file.Path, CompressionLevel.Optimal); entry.LastWriteTime = ZipEpoch;
                using var input = File.OpenRead(SafeFile(source, file.Path)); using var target = entry.Open(); input.CopyTo(target);
            }
        }
        return new() { Id = package.Id, Version = package.Version, Kind = package.Kind, Url = baseUrl + "/" + name,
            Size = new FileInfo(destination).Length, Sha256 = Hash(destination), Files = manifest };
    }
    static void VerifyPackage(string file, ResourcePackage p)
    {
        if (new FileInfo(file).Length != p.Size || !string.Equals(Hash(file), p.Sha256, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException($"Package hash or size mismatch: {p.Id}");
        using var zip = ZipFile.OpenRead(file);
        var entries = new Dictionary<string, ZipArchiveEntry>(StringComparer.OrdinalIgnoreCase);
        foreach (var e in zip.Entries)
        {
            SafeFile(Path.GetTempPath(), e.FullName);
            if (!entries.TryAdd(e.FullName, e) || (e.ExternalAttributes >> 16 & 0xF000) == 0xA000) throw new InvalidDataException("Duplicate ZIP entry or symbolic link.");
        }
        if (entries.Count != p.Files.Count) throw new InvalidDataException("Unexpected or missing ZIP files.");
        foreach (var f in p.Files)
        {
            if (!entries.TryGetValue(f.Path, out var e) || e.Length != f.Size) throw new InvalidDataException("ZIP file size mismatch.");
            using var stream = e.Open();
            if (!string.Equals(Convert.ToHexString(SHA256.HashData(stream)), f.Sha256, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("ZIP file hash mismatch.");
        }
    }
    static async Task Prepare(Dictionary<string, string> o)
    {
        var output = Path.GetFullPath(Required(o, "output"));
        if (Directory.Exists(output) && Directory.EnumerateFileSystemEntries(output).Any()) throw new IOException("Output directory must be empty; reviewed artifacts are immutable.");
        Directory.CreateDirectory(output);
        var appRoot = Path.GetFullPath(Required(o, "app-root"));
        var assets = Path.Combine(appRoot, "Assets");
        var snapshot = Read<ResourceSnapshot>(Path.Combine(assets, "Updates", "bundled-snapshot.json"));
        var build = Read<BuildInfo>(Path.Combine(appRoot, "build-info.json"));
        using var provenance = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(appRoot, "build-info.json")));
        var sourceDirty = !provenance.RootElement.TryGetProperty("sourceDirty", out var dirty) || dirty.GetBoolean();
        var sourceTreeSha256 = provenance.RootElement.TryGetProperty("sourceTreeSha256", out var treeHash) ? treeHash.GetString() : null;
        if (!Regex.IsMatch(build.SourceCommit, "^[a-f0-9]{40}$") || build.BaselineId != snapshot.BaselineId) throw new InvalidDataException("Build metadata must have a real source SHA and matching baseline.");
        var production = o.GetValueOrDefault("test") != "true";
        if (o.GetValueOrDefault("program-shards") == "true" && o.GetValueOrDefault("program-release") != "true")
            throw new ArgumentException("--program-shards only applies to a program release; pass --program-release true as well.");
        using var key = LoadPrivate(Required(o, "private-key"), production, out var keyId);
        var keys = Read<TrustedUpdateKeys>(Required(o, "public-key"));
        var trusted = keys.Keys.Single(k => k.KeyId == keyId);
        if (!CryptographicOperations.FixedTimeEquals(key.ExportSubjectPublicKeyInfo(), Convert.FromBase64String(trusted.PublicKey))) throw new CryptographicException("Signing key does not match public registry.");
        var sequence = long.Parse(Required(o, "sequence"));
        var version = Id(Required(o, "resource-version"));
        var tag = Id(Required(o, "tag"));
        var baseUrl = $"https://github.com/{RepoSlug}/releases/download/" + tag;
        UpdateCatalog? previous = null;
        if (o.TryGetValue("previous", out var previousFile))
        {
            previous = VerifyEnvelope(previousFile, keys, production);
            if (previous.Sequence >= sequence) throw new InvalidDataException("Catalog sequence must increase.");
        }
        var packages = new List<ResourcePackage>();
        var packagedScenes = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        // True when at least one resource package is new in this release. Published package bytes are reused
        // as they are, so a release that reuses every package ships exactly the resource set the previous
        // release already published.
        var resourceChanged = false;
        foreach (var p in snapshot.Packages)
        {
            var source = SafeFile(assets, p.Directory.Replace('\\', '/'));
            if (p.Kind == "tile")
            {
                using var manifest = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(source, "manifest.json")));
                if (!manifest.RootElement.GetProperty("referenceVerification").GetProperty("passed").GetBoolean()) throw new InvalidDataException("Cannot publish an unverified tile pack.");
                if (manifest.RootElement.TryGetProperty("scene", out var scene) && scene.ValueKind == JsonValueKind.String)
                    packagedScenes.Add(scene.GetString() ?? "");
            }
            var prior = previous?.Resources.SelectMany(r => r.Packages).LastOrDefault(q => q.Id == p.Id);
            if (prior is not null && prior.Kind != p.Kind) throw new InvalidDataException("A package ID cannot change its resource kind.");
            var built = Package(source, output, p with { Version = version }, baseUrl);
            // Identical file bytes retain their old package identity and URL, enabling true differential updates.
            if (prior is not null && FileListsEqual(prior.Files, built.Files))
            {
                var generated = Path.Combine(output, "packages", $"{built.Id}-{built.Version}.zip");
                if (!string.Equals(prior.Sha256, built.Sha256, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Unchanged source produced a different archive; use the matching deterministic packer.");
                var retained = Path.Combine(output, "packages", $"{prior.Id}-{prior.Version}.zip");
                if (!string.Equals(generated, retained, StringComparison.OrdinalIgnoreCase)) File.Move(generated, retained);
                built = prior;
            }
            else resourceChanged = true;
            if (previous?.Resources.SelectMany(r => r.Packages).Any(q => q.Id == built.Id && q.Version == built.Version && q.Sha256 != built.Sha256) == true) throw new InvalidDataException("Package version reuse with different content is forbidden.");
            packages.Add(built);
        }
        // The native loader no longer refuses a snapshot that is missing an approved scene's pack, because a
        // player who uninstalls that region produces exactly that state on purpose. The release-time accident
        // it used to catch — a scene approved for release whose pack was not shipped — is caught here instead,
        // where it belongs and where it can still be fixed.
        var approvalsPath = Path.Combine(SafeFile(assets, snapshot.MapDataRoot.Replace('\\', '/')), "scene-validation.json");
        if (File.Exists(approvalsPath))
        {
            using var approvals = JsonDocument.Parse(File.ReadAllBytes(approvalsPath));
            if (approvals.RootElement.TryGetProperty("scenes", out var scenes) && scenes.ValueKind == JsonValueKind.Object)
                foreach (var scene in scenes.EnumerateObject())
                    if (scene.Value.TryGetProperty("approved", out var approved) && approved.ValueKind == JsonValueKind.True &&
                        !packagedScenes.Contains(scene.Name))
                        throw new InvalidDataException($"已批准的场景没有随本资源发布瓦片包：{scene.Name}");
        }
        var release = new ResourceRelease { SnapshotId = "resources-" + version, Sequence = sequence, BaselineId = build.BaselineId,
            MinAppVersion = o.GetValueOrDefault("min-app-version", build.AppVersion), MaxAppVersion = o.GetValueOrDefault("max-app-version"),
            Notes = o.TryGetValue("notes-file", out var notesFile) ? File.ReadAllText(notesFile) : "地图资源更新", Packages = packages };
        var resources = previous?.Resources.Where(r => r.BaselineId != release.BaselineId).ToList() ?? [];
        resources.Add(release);
        var app = previous?.App ?? new ProgramRelease { Version = build.AppVersion, Url = $"https://github.com/{RepoSlug}/releases/tag/" + tag, Notes = "首次支持程序与地图资源更新。" };
        // Positive signal for the release script: only a preparation that built the program itself may
        // upload program archives; a resource-only release carries the published program forward.
        var programPrepared = false;
        if (o.GetValueOrDefault("program-release") == "true")
        {
            ProgramPackage? program = null;
            if (o.GetValueOrDefault("program-shards") == "true")
            {
                if (o.ContainsKey("program-zip")) throw new ArgumentException("A shard release inventories --app-root itself; do not also pass --program-zip.");
                program = await BuildProgramShards(appRoot, build, output, baseUrl, previous);
            }
            else if (o.TryGetValue("program-zip", out var programZip))
            {
                program = await ProgramPackageValidation.DescribeAsync(programZip, build, baseUrl + "/" + Uri.EscapeDataString(Path.GetFileName(programZip)));
                // Validate the exact ZIP bytes to be signed, rather than trusting a neighboring report.
                var verifiedProgram = Path.Combine(output, "program-verification");
                await ProgramPackageValidation.ExtractAsync(programZip, verifiedProgram, program);
                await ProgramPackageValidation.VerifyDirectoryAsync(verifiedProgram, new ProgramRelease { Version = build.AppVersion, Package = program });
            }
            else if (production) throw new InvalidDataException("Program releases require --program-zip with the complete tested application archive, or --program-shards true with --app-root.");
            // A published program version is immutable: the same version may not describe different bytes.
            if (program is not null && previous?.App.Version == build.AppVersion && previous.App.Package is not null && !SameProgramContent(previous.App.Package, program))
                throw new InvalidDataException("The published program version is immutable. Increment the version before rebuilding.");
            app = new() { Version = build.AppVersion, Url = $"https://github.com/{RepoSlug}/releases/tag/" + tag, Notes = release.Notes, Package = program };
            programPrepared = program is not null;
        }
        var catalog = new UpdateCatalog { Sequence = sequence, App = app, Resources = resources };
        ValidateCatalog(catalog);
        var signedFile = Path.Combine(output, "update.json");
        WriteNew(signedFile, Sign(catalog, key, keyId));
        VerifyEnvelope(signedFile, keys, production);
        foreach (var p in packages) VerifyPackage(Path.Combine(output, "packages", $"{p.Id}-{p.Version}.zip"), p);
        // Validate source snapshot using the same native parser used by installed clients. The bundled
        // descriptor is relative on purpose, so every root it names has to be made absolute here; the
        // optional ones are only present once a layout splits icons or base features into their own
        // package, and a relative value fails strict validation for the whole snapshot.
        var candidate = snapshot with { FormatVersion = 2, SnapshotId = release.SnapshotId, Sequence = sequence, Bundled = false,
            MinAppVersion = release.MinAppVersion, MaxAppVersion = release.MaxAppVersion,
            BaselineRoot = assets, MapDataRoot = Path.Combine(assets, snapshot.MapDataRoot),
            MapIconRoot = Absolute(assets, snapshot.MapIconRoot),
            MapFeatureRoot = Absolute(assets, snapshot.MapFeatureRoot),
            Packages = snapshot.Packages.Select((p, i) => p with { Directory = Path.Combine(assets, p.Directory), Version = packages[i].Version, Sha256 = packages[i].Sha256, Files = packages[i].Files }).ToList() };
        var candidateFile = Path.Combine(output, "preflight-snapshot.json");
        WriteNew(candidateFile, candidate);
        var nativePassed = false;
        if (o.TryGetValue("core-host", out var host))
        {
            var start = new ProcessStartInfo(Path.GetFullPath(host)) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true, WorkingDirectory = appRoot };
            start.ArgumentList.Add("--check-resource-snapshot"); start.ArgumentList.Add(candidateFile);
            using var process = Process.Start(start) ?? throw new IOException("Could not start CoreHost.");
            var stdout = process.StandardOutput.ReadToEndAsync(); var stderr = process.StandardError.ReadToEndAsync();
            using var timeout = new CancellationTokenSource(TimeSpan.FromMinutes(5));
            try { await process.WaitForExitAsync(timeout.Token); } catch { process.Kill(true); throw; }
            File.WriteAllText(Path.Combine(output, "native-check.stdout.json"), await stdout);
            File.WriteAllText(Path.Combine(output, "native-check.stderr.log"), await stderr);
            if (process.ExitCode != 0) throw new InvalidDataException("Native snapshot preflight failed; see output logs.");
            nativePassed = true;
        }
        if (production && !nativePassed) throw new InvalidOperationException("Production preparation requires --core-host and successful native preflight.");
        // An offline archive is one file carrying every package of this release, for players who import it
        // without any network access. When this release retains every published package, that file would
        // repeat the several hundred megabytes the previous release already published, so it is only built
        // when the resource set actually changed, or when the caller asks for it explicitly.
        var offlineNeeded = o.GetValueOrDefault("with-offline") == "true" || resourceChanged;
        var offline = Path.Combine(output, $"resources-{version}-offline.zip");
        if (offlineNeeded)
        {
            using (var zip = new ZipArchive(new FileStream(offline, FileMode.CreateNew), ZipArchiveMode.Create))
            {
                AddFile(zip, signedFile, "update.json", CompressionLevel.Optimal);
                foreach (var p in packages) { var name = $"{p.Id}-{p.Version}.zip"; AddFile(zip, Path.Combine(output, "packages", name), "packages/" + name, CompressionLevel.NoCompression); }
            }
            if (new FileInfo(offline).Length >= 2L * 1024 * 1024 * 1024) throw new InvalidOperationException("A GitHub Releases attachment must be smaller than 2 GiB. Split the resource distribution before publishing this release.");
        }
        else
        {
            Console.WriteLine("Every resource package is unchanged, so no offline archive was built; pass --with-offline true to build one anyway.");
        }
        if (packages.Any(p => p.Size >= 2L * 1024 * 1024 * 1024))
            throw new InvalidOperationException("A GitHub Releases attachment must be smaller than 2 GiB. Split the resource distribution before publishing this release.");
        WriteNew(Path.Combine(output, "release-report.json"), new { formatVersion = 1, production, sourceCommit = build.SourceCommit, sourceDirty, sourceTreeSha256, appVersion = build.AppVersion, baselineId = build.BaselineId,
            tag, sequence, snapshotId = release.SnapshotId, nativePassed, programPrepared, signedManifestSha256 = Hash(signedFile),
            assets = packages.Select(p => new { name = $"{p.Id}-{p.Version}.zip", sha256 = p.Sha256, size = p.Size, url = p.Url }).ToArray(),
            // A shard release publishes several attachments plus a descriptor; the release script binds each
            // one to the identity the signed catalog names, exactly as it does for resource packages.
            program = app.Package is null ? null : new { url = app.Package.Url, name = Path.GetFileName(new Uri(app.Package.Url).AbsolutePath), size = app.Package.Size, sha256 = app.Package.Sha256,
                files = app.Package.Files.Count, shards = app.Package.Shards.Select(s => new { id = s.Id, name = Path.GetFileName(new Uri(s.Url).AbsolutePath), url = s.Url, size = s.Size, sha256 = s.Sha256, files = s.Files.Count }).ToArray() },
            offline = offlineNeeded ? new { name = Path.GetFileName(offline), size = new FileInfo(offline).Length, sha256 = Hash(offline) } : null });
        Console.WriteLine($"Prepared {packages.Count} signed resource packages{(offlineNeeded ? ", offline archive" : "")} and validation report. No remote publication occurred.");
    }
    static bool FileListsEqual(List<ResourceFile> a, List<ResourceFile> b) => a.Count == b.Count && a.OrderBy(x => x.Path, StringComparer.Ordinal).SequenceEqual(b.OrderBy(x => x.Path, StringComparer.Ordinal));
    /// <summary>
    /// Whether a program release describes the same bytes as the one already published under that version.
    /// Shard URLs are not content, so a re-prepare of identical content under another tag is not a change.
    /// </summary>
    static bool SameProgramContent(ProgramPackage prior, ProgramPackage current) =>
        prior.Shards.Count > 0 && current.Shards.Count > 0 ? FileListsEqual(prior.Files, current.Files) : prior.Sha256 == current.Sha256;
    /// <summary>
    /// Inventories a staged program root and groups it by shard. An unclassified path, a missing required
    /// file or an empty shard is refused here, so nothing downstream has to guess whether the tree was
    /// complete or whether a new file quietly joined a large shard.
    /// </summary>
    static (List<ResourceFile> Files, Dictionary<string, List<ResourceFile>> Shards) ClassifyProgramRoot(string appRoot)
    {
        var root = Path.GetFullPath(appRoot);
        if (!Directory.Exists(root)) throw new IOException("App root does not exist.");
        var paths = Directory.GetFiles(root, "*", SearchOption.AllDirectories).OrderBy(p => Relative(root, p), StringComparer.Ordinal).ToArray();
        if (paths.Length == 0) throw new InvalidDataException("Empty app root.");
        var files = new List<ResourceFile>(paths.Length);
        foreach (var path in paths)
        {
            UpdateStorage.RejectLink(path);
            if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0) throw new InvalidDataException($"Unapproved link: {Relative(root, path)}");
            files.Add(new() { Path = Relative(root, path), Size = new FileInfo(path).Length, Sha256 = Hash(path) });
        }
        var (shards, unclassified) = ShardMap.Group(files);
        if (unclassified.Count > 0)
            throw new InvalidDataException($"{unclassified.Count} program file(s) have no shard rule, add them to ShardMap deliberately: {string.Join(", ", unclassified.Take(10))}");
        var staged = files.Select(f => f.Path).ToHashSet(StringComparer.OrdinalIgnoreCase);
        foreach (var required in ProgramPackageValidation.RequiredFiles)
            if (!staged.Contains(required)) throw new InvalidDataException($"App root is missing a required program file: {required}");
        foreach (var id in ShardMap.Ids)
            if (shards[id].Count == 0) throw new InvalidDataException($"Shard {id} has no files; update ShardMap if the layout genuinely changed.");
        return (files, shards);
    }
    /// <summary>
    /// Packs one shard with the rules that make identical content produce identical bytes: entries already
    /// sorted by path, a fixed timestamp and one compression level. The differential branch depends on it.
    /// </summary>
    static void PackShard(string destination, string appRoot, List<ResourceFile> files)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
        using var archive = new ZipArchive(new FileStream(destination, FileMode.CreateNew), ZipArchiveMode.Create);
        foreach (var file in files)
        {
            var entry = archive.CreateEntry(file.Path, CompressionLevel.Optimal);
            entry.LastWriteTime = ZipEpoch;
            using var input = File.OpenRead(SafeFile(appRoot, file.Path));
            using var target = entry.Open();
            input.CopyTo(target);
        }
    }
    static void VerifyShard(string file, ProgramPackage package, ProgramShard shard)
    {
        if (new FileInfo(file).Length != shard.Size || !string.Equals(Hash(file), shard.Sha256, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException($"Shard hash or size mismatch: {shard.Id}");
        var declared = package.Files.ToDictionary(f => f.Path, StringComparer.OrdinalIgnoreCase);
        using var zip = ZipFile.OpenRead(file);
        var entries = new Dictionary<string, ZipArchiveEntry>(StringComparer.OrdinalIgnoreCase);
        foreach (var e in zip.Entries)
        {
            SafeFile(Path.GetTempPath(), e.FullName);
            if (!entries.TryAdd(e.FullName, e) || (e.ExternalAttributes >> 16 & 0xF000) == 0xA000) throw new InvalidDataException("Duplicate ZIP entry or symbolic link.");
        }
        if (entries.Count != shard.Files.Count) throw new InvalidDataException($"Unexpected or missing ZIP files in shard {shard.Id}.");
        foreach (var path in shard.Files)
        {
            if (!declared.TryGetValue(path, out var expected)) throw new InvalidDataException($"Shard {shard.Id} names a file outside the signed inventory.");
            if (!entries.TryGetValue(path, out var e) || e.Length != expected.Size) throw new InvalidDataException($"ZIP file size mismatch in shard {shard.Id}.");
            using var stream = e.Open();
            if (!string.Equals(Convert.ToHexString(SHA256.HashData(stream)), expected.Sha256, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException($"ZIP file hash mismatch in shard {shard.Id}.");
        }
    }
    /// <summary>
    /// Turns a staged program root into the shard set of a signed program release. Shards whose files are
    /// unchanged keep the identity and URL they were published with, so they are neither re-uploaded nor
    /// re-downloaded. The release is only accepted after its shards have been reassembled and verified by
    /// the same verifier an installed program is checked with.
    /// </summary>
    static async Task<ProgramPackage> BuildProgramShards(string appRoot, BuildInfo build, string output, string baseUrl, UpdateCatalog? previous)
    {
        var (files, shards) = ClassifyProgramRoot(appRoot);
        var priorPackage = previous?.App.Package;
        var priorFiles = priorPackage?.Files.ToDictionary(f => f.Path, StringComparer.OrdinalIgnoreCase);
        var built = new List<ProgramShard>(ShardMap.Ids.Length);
        foreach (var id in ShardMap.Ids)
        {
            var prior = priorPackage?.Shards.FirstOrDefault(s => s.Id.Equals(id, StringComparison.OrdinalIgnoreCase));
            List<ResourceFile>? priorList = null;
            if (prior is not null && priorFiles is not null && prior.Files.Count == shards[id].Count)
            {
                priorList = new List<ResourceFile>(prior.Files.Count);
                foreach (var path in prior.Files)
                {
                    if (!priorFiles.TryGetValue(path, out var record)) { priorList = null; break; }
                    priorList.Add(record);
                }
            }
            var reusable = prior is not null && priorList is not null && FileListsEqual(priorList, shards[id]);
            // An identical shard keeps its published asset name, so a retained URL always names a real file.
            var name = reusable ? Path.GetFileName(new Uri(prior!.Url).AbsolutePath) : $"IMao-v{build.AppVersion}-{id}.zip";
            var destination = Path.Combine(output, "program", name);
            PackShard(destination, appRoot, shards[id]);
            var sha256 = Hash(destination);
            if (reusable)
            {
                if (!string.Equals(sha256, prior!.Sha256, StringComparison.OrdinalIgnoreCase) || new FileInfo(destination).Length != prior.Size)
                    throw new InvalidDataException($"Unchanged shard {id} produced a different archive; use the matching deterministic packer.");
                built.Add(prior);
                continue;
            }
            built.Add(new() { Id = id, Url = baseUrl + "/" + Uri.EscapeDataString(name), Size = new FileInfo(destination).Length, Sha256 = sha256, Files = shards[id].Select(f => f.Path).ToList() });
        }
        // Clients that predate shards follow the whole-package pointer, so it has to name real, hash-matched
        // bytes. It names this small descriptor instead of an archive that is never uploaded: such a client
        // downloads a few kilobytes and then refuses the release, instead of starting an 840 MB download.
        var pointerName = $"IMao-v{build.AppVersion}-shards.json";
        var pointerPath = Path.Combine(output, "program", pointerName);
        var program = new ProgramPackage { SourceCommit = build.SourceCommit, BaselineId = build.BaselineId, Url = baseUrl + "/" + Uri.EscapeDataString(pointerName), Files = files, Shards = built };
        WriteNew(pointerPath, new { formatVersion = 1, appVersion = build.AppVersion, sourceCommit = build.SourceCommit, baselineId = build.BaselineId,
            note = "程序以分片发布：不支持分片的旧客户端请从发行页手动安装完整程序包。",
            totalFiles = files.Count, totalSize = files.Sum(f => f.Size),
            shards = built.Select(s => new { id = s.Id, name = Path.GetFileName(new Uri(s.Url).AbsolutePath), url = s.Url, size = s.Size, sha256 = s.Sha256, files = s.Files.Count }).ToArray() });
        program = program with { Size = new FileInfo(pointerPath).Length, Sha256 = Hash(pointerPath) };
        ProgramPackageValidation.Validate(program);
        // The regression that makes shards safe: assemble the release from the shard archives that will be
        // uploaded and run the verifier every installed program is checked by on the result.
        var reassembly = Path.Combine(output, "program-reassembly");
        foreach (var shard in program.Shards)
            await ProgramPackageValidation.ExtractShardAsync(Path.Combine(output, "program", Path.GetFileName(new Uri(shard.Url).AbsolutePath)), reassembly, program, shard.Id);
        await ProgramPackageValidation.VerifyDirectoryAsync(reassembly, new ProgramRelease { Version = build.AppVersion, Package = program });
        Console.WriteLine($"Prepared {program.Shards.Count} program shards ({program.Shards.Sum(s => s.Size) / (1024.0 * 1024.0):N1} MB compressed) and verified their reassembly.");
        return program;
    }
    static void AddFile(ZipArchive zip, string source, string name, CompressionLevel compression)
    {
        var entry = zip.CreateEntry(name, compression); entry.LastWriteTime = ZipEpoch;
        using var input = File.OpenRead(source); using var output = entry.Open(); input.CopyTo(output);
    }
    static void Verify(Dictionary<string, string> o)
    {
        var root = Path.GetFullPath(Required(o, "input"));
        var catalog = VerifyEnvelope(Path.Combine(root, "update.json"), Read<TrustedUpdateKeys>(Required(o, "public-key")), o.GetValueOrDefault("test") != "true");
        var selected = o.TryGetValue("snapshot-id", out var id) ? catalog.Resources.Single(r => r.SnapshotId == id) : catalog.Resources.OrderByDescending(r => r.Sequence).First();
        foreach (var p in selected.Packages) VerifyPackage(Path.Combine(root, "packages", $"{p.Id}-{p.Version}.zip"), p);
        // A prepared output holds every shard it publishes, including the ones retained from an earlier
        // release, so each one can be checked against the identity the signed catalog names for it.
        if (catalog.App.Package is { Shards.Count: > 0 } program && Directory.Exists(Path.Combine(root, "program")))
        {
            foreach (var shard in program.Shards) VerifyShard(Path.Combine(root, "program", Path.GetFileName(new Uri(shard.Url).AbsolutePath)), program, shard);
            var pointer = Path.Combine(root, "program", Path.GetFileName(new Uri(program.Url).AbsolutePath));
            if (new FileInfo(pointer).Length != program.Size || !string.Equals(Hash(pointer), program.Sha256, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Program shard descriptor does not match the signed pointer.");
            Console.WriteLine($"Verified {program.Shards.Count} program shards and their descriptor.");
        }
        Console.WriteLine($"Verified signature and {selected.Packages.Count} complete resource packages for {selected.SnapshotId}.");
    }

    /// <summary>
    /// Classifies a staged program root with <see cref="ShardMap"/> and refuses anything the table does
    /// not recognize. This is the reviewable form of the shard boundaries: it runs on a real app root
    /// before any archive exists, so a boundary mistake is found while it still costs nothing.
    /// </summary>
    static void ShardMapReport(Dictionary<string, string> o)
    {
        var root = Path.GetFullPath(Required(o, "app-root"));
        var (files, shards) = ClassifyProgramRoot(root);
        var report = ShardMap.Ids.Select(id => new { id, files = shards[id].Count, size = shards[id].Sum(f => f.Size) }).ToArray();
        var total = report.Sum(r => r.size);
        Console.WriteLine($"Shard map for {root}");
        Console.WriteLine($"  {files.Count} files, {total / (1024.0 * 1024.0):N1} MB total");
        foreach (var entry in report) Console.WriteLine($"  {entry.id,-18} {entry.files,5} files {entry.size / (1024.0 * 1024.0),9:N2} MB");
        if (o.TryGetValue("report", out var reportPath))
            WriteNew(Path.GetFullPath(reportPath), new { formatVersion = 1, appRoot = root, totalFiles = files.Count, totalSize = total, shards = report });
    }
    static void SelfTest(string root)
    {
        root = Path.GetFullPath(root);
        if (Directory.Exists(root)) throw new IOException("Self-test output must not exist.");
        Directory.CreateDirectory(root);
        using var key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        var keys = new TrustedUpdateKeys { Keys = [new() { KeyId = "fixture", TestOnly = true, PublicKey = Convert.ToBase64String(key.ExportSubjectPublicKeyInfo()) }] };
        var source = Path.Combine(root, "source"); Directory.CreateDirectory(source); File.WriteAllText(Path.Combine(source, "sample.json"), "{\"id\":\"stable-point\"}");
        var p = Package(source, root, new() { Id = "map-data", Version = "2026.9.9.1", Kind = "map-data" }, "https://github.com/kahvia-d/WWMAP-TOOLS/releases/download/test");
        VerifyPackage(Path.Combine(root, "packages", "map-data-2026.9.9.1.zip"), p);
        var catalog = new UpdateCatalog { Sequence = 1, App = new() { Version = "2026.9.9.1", Url = "https://github.com/kahvia-d/WWMAP-TOOLS/releases/tag/test" }, Resources = [new() { SnapshotId = "test-1", Sequence = 1, BaselineId = "baseline", MinAppVersion = "2026.9.9.1", Packages = [p] }] };
        var envelopeFile = Path.Combine(root, "update.json"); WriteNew(envelopeFile, Sign(catalog, key, "fixture"));
        VerifyEnvelope(envelopeFile, keys, false);
        var passed = new List<string> { "deterministic resource package and file hashes", "P-256 signed envelope round-trip" };
        void Reject(string name, Action action) { try { action(); } catch { passed.Add(name); return; } throw new Exception("Expected rejection: " + name); }
        Reject("test signing key rejected for production", () => VerifyEnvelope(envelopeFile, keys, true));
        var altered = Read<SignedUpdateEnvelope>(envelopeFile) with { Payload = Convert.ToBase64String("{}"u8.ToArray()) };
        var alteredPath = Path.Combine(root, "tampered.json"); WriteNew(alteredPath, altered);
        Reject("tampered payload rejected", () => VerifyEnvelope(alteredPath, keys, false));
        Reject("path traversal rejected", () => SafeFile(root, "../escape.json"));
        Reject("executable extension rejected", () => { File.WriteAllText(Path.Combine(source, "bad.exe"), "bad"); Package(source, Path.Combine(root, "bad"), new() { Id = "map-data", Version = "2", Kind = "map-data" }, "https://github.com/kahvia-d/WWMAP-TOOLS/releases/download/test"); });
        Reject("unknown release host rejected", () => RequireGithub("https://example.com/kahvia-d/WWMAP-TOOLS/releases/download/test/a.zip", true));
        Reject("another repository's release rejected", () => RequireGithub("https://github.com/someone-else/IMAO/releases/download/test/a.zip", true));
        // The rename must not orphan anything already published under the old slug.
        RequireGithub("https://github.com/kahvia-d/IMAO/releases/download/test/a.zip", true);
        RequireGithub("https://github.com/kahvia-d/IMAO/releases/tag/v1", false);
        RequireGithub("https://github.com/kahvia-d/WWMAP-TOOLS/releases/download/test/a.zip", true);
        RequireGithub("https://github.com/kahvia-d/WWMAP-TOOLS/releases/tag/v1", false);
        passed.Add("both the current and the pre-rename repository slug are accepted for release URLs");
        Reject("missing map-data rejected", () => ValidateCatalog(catalog with { Resources = [catalog.Resources[0] with { Packages = [] }] }));
        Reject("reserved Windows filename rejected", () => SafeFile(root, "CON.json"));
        var originalZip = Path.Combine(root, "packages", "map-data-2026.9.9.1.zip");
        var duplicateZip = Path.Combine(root, "duplicate.zip");
        File.Copy(originalZip, duplicateZip);
        using (var zip = ZipFile.Open(duplicateZip, ZipArchiveMode.Update)) { using var extra = zip.CreateEntry("sample.json").Open(); extra.Write("{}"u8); }
        Reject("duplicate archive entry rejected", () => VerifyPackage(duplicateZip, p with { Size = new FileInfo(duplicateZip).Length, Sha256 = Hash(duplicateZip) }));
        var missingZip = Path.Combine(root, "missing.zip");
        using (var zip = ZipFile.Open(missingZip, ZipArchiveMode.Create)) { }
        Reject("missing archive file rejected", () => VerifyPackage(missingZip, p with { Size = new FileInfo(missingZip).Length, Sha256 = Hash(missingZip) }));
        Reject("modified archive hash rejected", () => VerifyPackage(duplicateZip, p));
        var protectedBytes = Dpapi.Protect("fixture-secret"u8.ToArray());
        if (!Dpapi.Unprotect(protectedBytes).AsSpan().SequenceEqual("fixture-secret"u8)) throw new Exception("DPAPI round-trip failed.");
        passed.Add("DPAPI CurrentUser round-trip");
        // A real producer round-trip proves unchanged map-data keeps its identity when only a feature changes.
        var app = Path.Combine(root, "fixture-app");
        Directory.CreateDirectory(Path.Combine(app, "Assets", "KuroMap"));
        Directory.CreateDirectory(Path.Combine(app, "Assets", "candidate"));
        File.WriteAllText(Path.Combine(app, "Assets", "KuroMap", "points.json"), "{\"id\":\"stable-point\"}");
        File.WriteAllText(Path.Combine(app, "Assets", "candidate", "visual-index.imx"), "fixture index 1");
        WriteNew(Path.Combine(app, "build-info.json"), new { appVersion = "2026.9.9.1", baselineId = "test-baseline", sourceCommit = new string('a', 40), sourceDirty = true });
        WriteNew(Path.Combine(app, "Assets", "Updates", "bundled-snapshot.json"), new ResourceSnapshot { SnapshotId = "bundled-test", BaselineId = "test-baseline", BaselineRoot = ".", MapDataRoot = "KuroMap", Bundled = true,
            Packages = [new() { Id = "map-data", Kind = "map-data", Version = "2026.9.9.1", Directory = "KuroMap" }, new() { Id = "fixture-feature", Kind = "candidate", Version = "2026.9.9.1", Directory = "candidate" }] });
        var privateFile = Path.Combine(root, "fixture-private.json");
        var publicFile = Path.Combine(root, "fixture-public.json");
        var privateBytes = key.ExportPkcs8PrivateKey();
        try { WriteNew(privateFile, new PrivateKey("fixture", true, Convert.ToBase64String(Dpapi.Protect(privateBytes)))); }
        finally { CryptographicOperations.ZeroMemory(privateBytes); }
        WriteNew(publicFile, keys);
        var options = new Dictionary<string, string> { ["app-root"] = app, ["private-key"] = privateFile, ["public-key"] = publicFile, ["output"] = Path.Combine(root, "first"), ["sequence"] = "1", ["resource-version"] = "2026.9.9.1", ["tag"] = "fixture-1", ["test"] = "true" };
        Prepare(options).GetAwaiter().GetResult();
        File.WriteAllText(Path.Combine(app, "Assets", "candidate", "visual-index.imx"), "fixture index 2");
        options["previous"] = Path.Combine(root, "first", "update.json"); options["output"] = Path.Combine(root, "second"); options["sequence"] = "2"; options["resource-version"] = "2026.9.9.2"; options["tag"] = "fixture-2";
        Prepare(options).GetAwaiter().GetResult();
        var second = VerifyEnvelope(Path.Combine(root, "second", "update.json"), keys, false);
        var preflight = Read<ResourceSnapshot>(Path.Combine(root, "second", "preflight-snapshot.json"));
        if (preflight.FormatVersion != 2 || preflight.Bundled || preflight.MinAppVersion != second.Resources.Single().MinAppVersion || preflight.MaxAppVersion != second.Resources.Single().MaxAppVersion)
            throw new Exception("Native preflight must carry the external snapshot schema and program compatibility bounds.");
        passed.Add("external v2 preflight preserves signed program compatibility bounds");
        // The native loader no longer refuses a snapshot that is missing an approved scene's pack, because a
        // player who uninstalls that region produces that state on purpose. The release-time accident it used
        // to catch has to still be caught, here.
        File.WriteAllText(Path.Combine(app, "Assets", "KuroMap", "scene-validation.json"),
            "{\"formatVersion\":1,\"scenes\":{\"Darkplain\":{\"approved\":true}}}");
        options["output"] = Path.Combine(root, "approved-without-pack");
        Reject("approved scene without a shipped pack is refused", () => Prepare(options).GetAwaiter().GetResult());
        File.Delete(Path.Combine(app, "Assets", "KuroMap", "scene-validation.json"));
        passed.Add("approved scene without a shipped pack is refused at release time");
        if (second.Resources.Single().Packages.Single(p => p.Id == "map-data").Version != "2026.9.9.1" || second.Resources.Single().Packages.Single(p => p.Id == "fixture-feature").Version != "2026.9.9.2") throw new Exception("Differential package identity preservation failed.");
        if (second.App.Url != $"https://github.com/{RepoSlug}/releases/tag/fixture-1") throw new Exception("Resource-only release changed program identity.");
        using (var offline = ZipFile.OpenRead(Path.Combine(root, "second", "resources-2026.9.9.2-offline.zip")))
            if (offline.Entries.Count != 3 || offline.GetEntry("update.json") is null || offline.GetEntry("packages/map-data-2026.9.9.1.zip") is null) throw new Exception("Offline archive is incomplete.");
        passed.Add("feature-only publish retains unchanged map-data version/hash and old program release");
        passed.Add("offline archive includes signed catalog and all selected packages");
        foreach (var file in ProgramPackageValidation.RequiredFiles)
        {
            var path = Path.Combine(app, file); Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            if (!File.Exists(path)) File.WriteAllText(path, "fixture:" + file);
        }
        var programZip = Path.Combine(root, "fixture-program.zip"); ZipFile.CreateFromDirectory(app, programZip);
        options["previous"] = Path.Combine(root, "second", "update.json"); options["output"] = Path.Combine(root, "program"); options["sequence"] = "3"; options["resource-version"] = "2026.9.9.3";
        options["program-release"] = "true"; options["program-zip"] = programZip;
        Prepare(options).GetAwaiter().GetResult();
        var programCatalog = VerifyEnvelope(Path.Combine(root, "program", "update.json"), keys, false);
        if (programCatalog.App.Package?.Sha256 != Hash(programZip) || programCatalog.App.Package.Files.Count != Directory.GetFiles(app, "*", SearchOption.AllDirectories).Length)
            throw new Exception("Program ZIP was not completely bound by the signature.");
        passed.Add("program release signs archive hash and complete executable inventory");
        options["previous"] = Path.Combine(root, "program", "update.json"); options["output"] = Path.Combine(root, "retained-program"); options["sequence"] = "4"; options["resource-version"] = "2026.9.9.4";
        options.Remove("program-release"); options.Remove("program-zip");
        Prepare(options).GetAwaiter().GetResult();
        if (VerifyEnvelope(Path.Combine(root, "retained-program", "update.json"), keys, false).App.Package?.Sha256 != programCatalog.App.Package!.Sha256) throw new Exception("Resource-only release lost signed program metadata.");
        passed.Add("resource-only release preserves signed program inventory");
        // A release that retains every published package ships exactly the resource set the previous release
        // already published, so it must not rebuild the offline archive; an explicit request still does.
        if (Read<JsonElement>(Path.Combine(root, "retained-program", "release-report.json")).GetProperty("offline").ValueKind != JsonValueKind.Null)
            throw new Exception("A release with no resource change must not build another offline archive.");
        if (File.Exists(Path.Combine(root, "retained-program", "resources-2026.9.9.4-offline.zip")))
            throw new Exception("A release with no resource change must not write an offline archive.");
        passed.Add("a release whose resource packages are all retained builds no offline archive");
        options["previous"] = Path.Combine(root, "second", "update.json"); options["output"] = Path.Combine(root, "forced-offline"); options["sequence"] = "9"; options["with-offline"] = "true";
        Prepare(options).GetAwaiter().GetResult();
        if (!File.Exists(Path.Combine(root, "forced-offline", "resources-2026.9.9.4-offline.zip")))
            throw new Exception("--with-offline must build the offline archive on request.");
        passed.Add("--with-offline builds the offline archive on request");
        options.Remove("with-offline");
        // Shard boundaries decide what every future program update has to download, so the table itself
        // is asserted here, before any archive exists.
        // Every file whose bytes are stamped with the release version has to sit in the smallest shard.
        // One of them landing in runtime would make every release re-download that whole shard.
        foreach (var stamped in new[] { "build-info.json", "launcher-build-info.json", "Assets/Updates/bundled-snapshot.json", "Assets/Updates/trusted-keys.json",
            "IMao-WinUI.exe", "IMao-WinUI.dll", "IMao-WinUI.Core.dll", "IMao-WinUI.deps.json", "IMao-WinUI.runtimeconfig.json", "KuroSyncBridge.exe", "IMao-Launcher.exe", "resources.pri" })
            if (ShardMap.IdFor(stamped) != ShardMap.Ui) throw new Exception($"A file that changes with every release is not in the ui shard: {stamped}");
        foreach (var required in ProgramPackageValidation.RequiredFiles)
            if (ShardMap.IdFor(required) is null) throw new Exception($"Required file has no shard rule: {required}");
        if (ShardMap.IdFor("IMao-CoreHost.exe") != ShardMap.Core || ShardMap.IdFor("common.dll") != ShardMap.Core) throw new Exception("CoreHost shard mapping changed.");
        if (ShardMap.IdFor("System.Private.CoreLib.dll") != ShardMap.Runtime || ShardMap.IdFor("paddle_inference.dll") != ShardMap.Runtime) throw new Exception("Native runtime payload must map to the runtime shard.");
        if (ShardMap.IdFor("zh-CN/Microsoft.ui.xaml.dll.mui") != ShardMap.Runtime || ShardMap.IdFor("Microsoft.UI.Xaml/foo.xbf") != ShardMap.Runtime) throw new Exception("Locale and Windows App SDK payload must map to the runtime shard.");
        if (ShardMap.IdFor("Assets/FeaturesDatas/KuroTilePacks/Darkplain/features.imf") != ShardMap.AssetsTiles || ShardMap.IdFor("Assets/FeaturesDatas/kuro-tile-packs.json") != ShardMap.AssetsTiles)
            throw new Exception("Tile packs and their registry must map to the tile shard.");
        if (ShardMap.IdFor("Assets/KuroMap/points.json") != ShardMap.AssetsMapData || ShardMap.IdFor("Assets/KuroMapIcons/icons/a.png") != ShardMap.AssetsMapIcons)
            throw new Exception("Map data and map icons must stay in separate shards.");
        if (ShardMap.IdFor("Assets/models/ocr.json") != ShardMap.AssetsMisc || ShardMap.IdFor("Assets/Fonts/a.ttf") != ShardMap.AssetsMisc || ShardMap.IdFor("Assets/th.jpg") != ShardMap.AssetsMisc)
            throw new Exception("Miscellaneous assets must share one shard.");
        if (ShardMap.IdFor("IMao-WinUI.Unknown.dll") is not null || ShardMap.IdFor("KuroSyncBridge.Unknown.exe") is not null || ShardMap.IdFor("Assets/newdir/file.json") is not null || ShardMap.IdFor("Assets/KuroMapExtra/file.json") is not null)
            throw new Exception("Unclassified files must not be assigned to a shard.");
        passed.Add("shard table pins every release-changing file to the smallest shard and refuses unknown paths");
        var shardApp = Path.Combine(root, "shard-app");
        foreach (var sample in new[] { "IMao-WinUI.exe", "IMao-CoreHost.exe", "common.dll", "System.Private.CoreLib.dll", "paddle_inference.dll", "zh-CN/app.mui",
            "Assets/KuroMap/points.json", "Assets/KuroMapIcons/icons/a.png", "Assets/FeaturesDatas/KuroTilePacks/Darkplain/features.imf",
            "Assets/FeaturesDatas/kuro-tile-packs.json", "Assets/models/ocr.json", "Assets/Fonts/a.ttf", "Assets/th.jpg" })
        {
            var path = Path.Combine(shardApp, sample); Directory.CreateDirectory(Path.GetDirectoryName(path)!); File.WriteAllText(path, "fixture:" + sample);
        }
        foreach (var required in ProgramPackageValidation.RequiredFiles)
        {
            var path = Path.Combine(shardApp, required); Directory.CreateDirectory(Path.GetDirectoryName(path)!); File.WriteAllText(path, "fixture:" + required);
        }
        var shardReportFile = Path.Combine(root, "shard-report.json");
        ShardMapReport(new Dictionary<string, string> { ["app-root"] = shardApp, ["report"] = shardReportFile });
        var shardReport = Read<JsonElement>(shardReportFile);
        var shardRows = shardReport.GetProperty("shards").EnumerateArray().ToDictionary(r => r.GetProperty("id").GetString()!, r => r.GetProperty("files").GetInt32());
        var shardAppFiles = Directory.GetFiles(shardApp, "*", SearchOption.AllDirectories).Length;
        if (shardRows.Count != ShardMap.Ids.Length || shardRows.Values.Sum() != shardAppFiles || shardRows.Values.Any(v => v == 0))
            throw new Exception("Shard map report must classify every staged file exactly once across every shard.");
        passed.Add("shard map covers a fixture program root and reports every shard");
        Reject("app root with an unclassified project file is refused", () => ShardMapReport(new Dictionary<string, string> { ["app-root"] = app }));
        // A shard program release inventories the staged root itself and only signs after reassembling the
        // shards it will upload, so the fixture needs the descriptors the real staging writes.
        var shardReleaseApp = Path.Combine(root, "shard-release-app");
        foreach (var sample in new[] { "IMao-WinUI.exe", "IMao-WinUI.dll", "IMao-Launcher.exe", "IMao-CoreHost.exe", "common.dll", "System.Private.CoreLib.dll", "paddle_inference.dll",
            "zh-CN/app.mui", "Assets/KuroMap/points.json", "Assets/KuroMapIcons/icons/a.png", "Assets/FeaturesDatas/KuroTilePacks/Darkplain/features.imf",
            "Assets/models/ocr.json", "Assets/Fonts/a.ttf", "Assets/th.jpg" })
        {
            var path = Path.Combine(shardReleaseApp, sample); Directory.CreateDirectory(Path.GetDirectoryName(path)!); File.WriteAllText(path, "fixture:" + sample);
        }
        var shardBuildFile = Path.Combine(shardReleaseApp, "build-info.json");
        File.WriteAllText(shardBuildFile, JsonSerializer.Serialize(new { appVersion = "2026.9.9.5", baselineId = "test-baseline", sourceCommit = new string('a', 40), sourceDirty = true }, Json));
        WriteNew(Path.Combine(shardReleaseApp, "Assets", "Updates", "bundled-snapshot.json"), new ResourceSnapshot { SnapshotId = "bundled-test", BaselineId = "test-baseline", BaselineRoot = ".", MapDataRoot = "KuroMap", Bundled = true,
            Packages = [new() { Id = "map-data", Kind = "map-data", Version = "2026.9.9.5", Directory = "KuroMap" }] });
        WriteNew(Path.Combine(shardReleaseApp, "Assets", "Updates", "trusted-keys.json"), keys);
        var shardOptions = new Dictionary<string, string>(options) { ["app-root"] = shardReleaseApp, ["output"] = Path.Combine(root, "shard-release"), ["previous"] = Path.Combine(root, "retained-program", "update.json"),
            ["sequence"] = "5", ["resource-version"] = "2026.9.9.5", ["tag"] = "fixture-3", ["program-release"] = "true", ["program-shards"] = "true" };
        shardOptions.Remove("program-zip");
        Prepare(shardOptions).GetAwaiter().GetResult();
        var shardCatalog = VerifyEnvelope(Path.Combine(root, "shard-release", "update.json"), keys, false);
        var shardPackage = shardCatalog.App.Package ?? throw new Exception("A shard release must sign a program package.");
        var inventory = shardPackage.Files.Select(f => f.Path).ToHashSet(StringComparer.OrdinalIgnoreCase);
        var covered = shardPackage.Shards.SelectMany(s => s.Files).ToList();
        if (shardPackage.Shards.Count != ShardMap.Ids.Length || covered.Count != inventory.Count || !covered.ToHashSet(StringComparer.OrdinalIgnoreCase).SetEquals(inventory))
            throw new Exception("Signed shards must partition the signed program file list exactly.");
        var pointerName = $"IMao-v{shardCatalog.App.Version}-shards.json";
        if (shardPackage.Url != $"https://github.com/{RepoSlug}/releases/download/fixture-3/{pointerName}" ||
            shardPackage.Size != new FileInfo(Path.Combine(root, "shard-release", "program", pointerName)).Length)
            throw new Exception("The whole-package pointer must name the real shard descriptor under this tag.");
        foreach (var shard in shardPackage.Shards) VerifyShard(Path.Combine(root, "shard-release", "program", Path.GetFileName(new Uri(shard.Url).AbsolutePath)), shardPackage, shard);
        if (!File.Exists(Path.Combine(root, "shard-release", "program-reassembly", "build-info.json")))
            throw new Exception("A shard release must be verified by reassembling its shards.");
        passed.Add("shard program release signs a complete partition and reassembles into a verified tree");
        // Differential reuse: every shard is unchanged, so every shard keeps the URL it was published with
        // even though this release has a new tag and sequence.
        shardOptions["previous"] = Path.Combine(root, "shard-release", "update.json"); shardOptions["output"] = Path.Combine(root, "shard-release-2");
        shardOptions["sequence"] = "6"; shardOptions["tag"] = "fixture-4"; shardOptions["resource-version"] = "2026.9.9.6";
        Prepare(shardOptions).GetAwaiter().GetResult();
        var reused = VerifyEnvelope(Path.Combine(root, "shard-release-2", "update.json"), keys, false).App.Package!;
        if (!reused.Shards.Select(s => s.Url).SequenceEqual(shardPackage.Shards.Select(s => s.Url)))
            throw new Exception("An unchanged shard must keep the URL it was published with.");
        passed.Add("unchanged shards retain their published URL under a new tag");
        // Only the shard whose content changed may move to the new release.
        File.WriteAllText(Path.Combine(shardReleaseApp, "Assets", "KuroMap", "points.json"), "{\"changed\":true}");
        File.WriteAllText(shardBuildFile, JsonSerializer.Serialize(new { appVersion = "2026.9.9.6", baselineId = "test-baseline", sourceCommit = new string('a', 40), sourceDirty = true }, Json));
        shardOptions["previous"] = Path.Combine(root, "shard-release-2", "update.json"); shardOptions["output"] = Path.Combine(root, "shard-release-3");
        shardOptions["sequence"] = "7"; shardOptions["tag"] = "fixture-5"; shardOptions["resource-version"] = "2026.9.9.7";
        Prepare(shardOptions).GetAwaiter().GetResult();
        var changed = VerifyEnvelope(Path.Combine(root, "shard-release-3", "update.json"), keys, false).App.Package!;
        // build-info.json is stamped with the version, so the ui shard has to move with every release; the
        // map-data shard moves because its content changed; everything else stays on its published URL.
        var expectedMoves = new[] { ShardMap.AssetsMapData, ShardMap.Ui }.OrderBy(x => x, StringComparer.Ordinal).ToArray();
        var moved = changed.Shards.Where(s => s.Url.Contains("/fixture-5/", StringComparison.Ordinal)).Select(s => s.Id).OrderBy(x => x, StringComparer.Ordinal).ToArray();
        if (!moved.SequenceEqual(expectedMoves)) throw new Exception("Only the changed shard and the version-stamped ui shard may move: " + string.Join(", ", moved));
        foreach (var shard in changed.Shards.Where(s => !expectedMoves.Contains(s.Id)))
            if (!shard.Url.Contains("/fixture-3/", StringComparison.Ordinal)) throw new Exception($"Unchanged shard {shard.Id} lost its published URL: {shard.Url}");
        passed.Add("only the changed shard and the version-stamped ui shard move to the new release tag");
        // Republishing the same program version with different bytes stays refused, with shards present.
        File.WriteAllText(Path.Combine(shardReleaseApp, "Assets", "KuroMap", "points.json"), "{\"changed\":\"again\"}");
        shardOptions["previous"] = Path.Combine(root, "shard-release-3", "update.json"); shardOptions["output"] = Path.Combine(root, "shard-release-4"); shardOptions["sequence"] = "8";
        Reject("republishing a program version with different shard content is refused", () => Prepare(shardOptions).GetAwaiter().GetResult());
        options["output"] = Path.Combine(root, "test-key-production"); options["test"] = "false";
        Reject("production prepare rejects test private key", () => Prepare(options).GetAwaiter().GetResult());
        WriteNew(Path.Combine(root, "test-report.json"), new { passed = passed.Count, tests = passed });
        Console.WriteLine($"PASS {passed.Count} publisher checks.");
    }
    sealed record PrivateKey(string KeyId, bool TestOnly, string ProtectedPkcs8);
}

static class Dpapi
{
    [StructLayout(LayoutKind.Sequential)] struct Blob { public int Length; public IntPtr Data; }
    [DllImport("crypt32.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern bool CryptProtectData(ref Blob input, string? description, IntPtr entropy, IntPtr reserved, IntPtr prompt, int flags, out Blob output);
    [DllImport("crypt32.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern bool CryptUnprotectData(ref Blob input, IntPtr description, IntPtr entropy, IntPtr reserved, IntPtr prompt, int flags, out Blob output);
    [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr pointer);
    public static byte[] Protect(byte[] input) => Transform(input, true);
    public static byte[] Unprotect(byte[] input) => Transform(input, false);
    static byte[] Transform(byte[] input, bool encrypt)
    {
        if (!OperatingSystem.IsWindows()) throw new PlatformNotSupportedException("Signing key protection requires Windows DPAPI CurrentUser.");
        var blob = new Blob { Length = input.Length, Data = Marshal.AllocHGlobal(input.Length) };
        Marshal.Copy(input, 0, blob.Data, input.Length);
        try
        {
            Blob output;
            var ok = encrypt ? CryptProtectData(ref blob, "WWMAP-TOOLS update signing key", IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, 1, out output)
                : CryptUnprotectData(ref blob, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, 1, out output);
            if (!ok) throw new CryptographicException(Marshal.GetLastWin32Error());
            try { var bytes = new byte[output.Length]; Marshal.Copy(output.Data, bytes, 0, bytes.Length); return bytes; }
            finally { for (var i = 0; i < output.Length; i++) Marshal.WriteByte(output.Data, i, 0); LocalFree(output.Data); }
        }
        finally { for (var i = 0; i < input.Length; i++) Marshal.WriteByte(blob.Data, i, 0); Marshal.FreeHGlobal(blob.Data); }
    }
}
