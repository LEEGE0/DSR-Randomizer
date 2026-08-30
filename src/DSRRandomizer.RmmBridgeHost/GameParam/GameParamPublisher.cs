using System.Security.Cryptography;
using System.Text.Json;
using SoulsFormats;

namespace DSRRandomizer.RmmBridgeHost.GameParam;

public sealed class GameParamPublisher : IGameParamPublisher
{
    public const string DependencyCommit = "55b08a3c02a03777cf19958d8f6aa18d7af59da1";
    private const int ManifestSchemaVersion = 1;
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = true,
    };

    private readonly GameParamThreeWayMerger _merger;
    private readonly Func<GameParamSourceSet> _resolveSources;
    private readonly Action<string> _log;

    public GameParamPublisher(
        GameParamThreeWayMerger merger,
        GameParamSourceResolver resolver,
        string externalRoot,
        string runtimeId,
        string sourceInstallationRoot,
        Action<string>? log = null)
        : this(
            merger,
            () => resolver.Resolve(externalRoot, runtimeId, sourceInstallationRoot),
            log)
    {
    }

    public GameParamPublisher(
        GameParamThreeWayMerger merger,
        GameParamSourceSet sources,
        Action<string>? log = null)
        : this(merger, () => sources, log)
    {
    }

    private GameParamPublisher(
        GameParamThreeWayMerger merger,
        Func<GameParamSourceSet> resolveSources,
        Action<string>? log)
    {
        _merger = merger ?? throw new ArgumentNullException(nameof(merger));
        _resolveSources = resolveSources ?? throw new ArgumentNullException(nameof(resolveSources));
        _log = log ?? (_ => { });
    }

    public Task PublishAsync(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        try
        {
            Publish(cancellationToken);
            return Task.CompletedTask;
        }
        catch (Exception exception)
        {
            _log($"gameparam status=failure error={exception}");
            throw;
        }
    }

    private void Publish(CancellationToken cancellationToken)
    {
        GameParamSourceSet sources = _resolveSources();
        Directory.CreateDirectory(sources.OutputDirectory);
        SourceHash[] sourceHashes = HashSources(sources, cancellationToken);

        if (CanReuse(sources, sourceHashes, cancellationToken))
        {
            _log(BuildLogEntry(sources, sourceHashes, "hit", null, Sha256(sources.OutputPath)));
            return;
        }

        string token = Guid.NewGuid().ToString("N");
        string outputStaging = Path.Combine(
            sources.OutputDirectory,
            $"GameParam.parambnd.dcx.staging-{token}");
        string manifestStaging = Path.Combine(
            sources.OutputDirectory,
            $"gameparam-merge-manifest.json.staging-{token}");
        try
        {
            GameParamMergeResult merge = MergeSources(sources);
            WriteDurableNewFile(outputStaging, merge.OutputBytes);
            VerifySemanticOutput(sources, outputStaging);
            SourceHash[] afterHashes = HashSources(sources, cancellationToken);
            if (!sourceHashes.SequenceEqual(afterHashes))
                throw new IOException("A GameParam source changed during publication.");

            string outputHash = Sha256(outputStaging);
            var manifest = new MergeManifest(
                ManifestSchemaVersion,
                DependencyCommit,
                sourceHashes,
                outputHash,
                new MergeCountManifest(
                    merge.ChangedEntries,
                    merge.AddedRows,
                    merge.ChangedRows,
                    merge.DeletedRows,
                    merge.PreservedTargetRows,
                    merge.RandomizerWinsOverlaps));
            WriteDurableNewFile(manifestStaging, JsonSerializer.SerializeToUtf8Bytes(manifest, JsonOptions));

            ReplaceAtomically(outputStaging, sources.OutputPath);
            ReplaceAtomically(manifestStaging, sources.ManifestPath);
            VerifySemanticOutput(sources, sources.OutputPath);
            if (!Sha256(sources.OutputPath).Equals(outputHash, StringComparison.Ordinal))
                throw new IOException("The published GameParam hash changed after atomic replacement.");
            _log(BuildLogEntry(sources, sourceHashes, "miss", merge, outputHash));
        }
        finally
        {
            DeleteExactStaging(outputStaging);
            DeleteExactStaging(manifestStaging);
        }
    }

    private bool CanReuse(
        GameParamSourceSet sources,
        SourceHash[] sourceHashes,
        CancellationToken cancellationToken)
    {
        if (!File.Exists(sources.OutputPath) || !File.Exists(sources.ManifestPath))
            return false;
        try
        {
            cancellationToken.ThrowIfCancellationRequested();
            MergeManifest manifest = ReadStrictManifest(sources.ManifestPath);
            return manifest.SchemaVersion == ManifestSchemaVersion
                && manifest.DependencyCommit.Equals(DependencyCommit, StringComparison.Ordinal)
                && manifest.Sources.SequenceEqual(sourceHashes)
                && manifest.OutputSha256.Equals(Sha256(sources.OutputPath), StringComparison.Ordinal)
                && VerifySemanticOutput(sources, sources.OutputPath);
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (Exception exception) when (
            exception is IOException or UnauthorizedAccessException or JsonException
                or InvalidDataException or FormatException or EndOfStreamException)
        {
            return false;
        }
    }

    private GameParamMergeResult MergeSources(GameParamSourceSet sources)
    {
        IReadOnlyCollection<PARAMDEF> definitions = LoadDefinitions(sources.DefinitionPaths);
        return _merger.Merge(new GameParamMergeInputs(
            BND3.Read(sources.BasePath),
            BND3.Read(sources.RandomizedPath),
            BND3.Read(sources.OverhaulPath),
            definitions));
    }

    private bool VerifySemanticOutput(GameParamSourceSet sources, string candidatePath)
    {
        byte[] candidateBytes = File.ReadAllBytes(candidatePath);
        BND3 candidate = BND3.Read(candidateBytes);
        GameParamMergeResult reapplied = _merger.Merge(new GameParamMergeInputs(
            BND3.Read(sources.BasePath),
            BND3.Read(sources.RandomizedPath),
            candidate,
            LoadDefinitions(sources.DefinitionPaths)));
        if (!candidateBytes.AsSpan().SequenceEqual(reapplied.OutputBytes))
            throw new InvalidDataException("The staged GameParam does not satisfy the three-way merge on semantic reopen.");
        return true;
    }

    private static PARAMDEF[] LoadDefinitions(IReadOnlyList<string> paths) =>
        paths.Select(path => PARAMDEF.XmlDeserialize(path)).ToArray();

    private static SourceHash[] HashSources(
        GameParamSourceSet sources,
        CancellationToken cancellationToken)
    {
        var hashes = new List<SourceHash>
        {
            new("base", sources.BasePath, Sha256(sources.BasePath)),
            new("randomized", sources.RandomizedPath, Sha256(sources.RandomizedPath)),
            new("overhaul", sources.OverhaulPath, Sha256(sources.OverhaulPath)),
        };
        foreach (string definition in sources.DefinitionPaths)
        {
            cancellationToken.ThrowIfCancellationRequested();
            hashes.Add(new SourceHash("paramdef", definition, Sha256(definition)));
        }
        return hashes.ToArray();
    }

    private static string Sha256(string path)
    {
        using var stream = new FileStream(path, new FileStreamOptions
        {
            Access = FileAccess.Read,
            Mode = FileMode.Open,
            Share = FileShare.Read,
            Options = FileOptions.SequentialScan,
        });
        return Convert.ToHexString(SHA256.HashData(stream)).ToLowerInvariant();
    }

    private static MergeManifest ReadStrictManifest(string path)
    {
        byte[] bytes = File.ReadAllBytes(path);
        using JsonDocument document = JsonDocument.Parse(bytes, new JsonDocumentOptions
        {
            AllowTrailingCommas = false,
            CommentHandling = JsonCommentHandling.Disallow,
            MaxDepth = 8,
        });
        RequireObjectProperties(
            document.RootElement,
            ["schemaVersion", "dependencyCommit", "sources", "outputSha256", "counts"]);
        JsonElement sources = document.RootElement.GetProperty("sources");
        if (sources.ValueKind != JsonValueKind.Array)
            throw new JsonException("Manifest sources must be an array.");
        foreach (JsonElement source in sources.EnumerateArray())
            RequireObjectProperties(source, ["kind", "path", "sha256"]);
        RequireObjectProperties(
            document.RootElement.GetProperty("counts"),
            ["changedEntries", "addedRows", "changedRows", "deletedRows", "preservedTargetRows", "randomizerWinsOverlaps"]);
        return JsonSerializer.Deserialize<MergeManifest>(bytes, JsonOptions)
            ?? throw new JsonException("The GameParam manifest is empty.");
    }

    private static void RequireObjectProperties(JsonElement element, IReadOnlyCollection<string> expected)
    {
        if (element.ValueKind != JsonValueKind.Object)
            throw new JsonException("A GameParam manifest value is not an object.");
        var names = new HashSet<string>(StringComparer.Ordinal);
        foreach (JsonProperty property in element.EnumerateObject())
        {
            if (!names.Add(property.Name))
                throw new JsonException($"Duplicate GameParam manifest property: {property.Name}");
        }
        if (!names.SetEquals(expected))
            throw new JsonException("GameParam manifest properties do not match the schema.");
    }

    private static void WriteDurableNewFile(string path, byte[] bytes)
    {
        using var stream = new FileStream(path, new FileStreamOptions
        {
            Access = FileAccess.Write,
            Mode = FileMode.CreateNew,
            Share = FileShare.None,
            Options = FileOptions.WriteThrough,
        });
        stream.Write(bytes);
        stream.Flush(flushToDisk: true);
    }

    private static void ReplaceAtomically(string stagingPath, string destinationPath)
    {
        if (File.Exists(destinationPath))
            File.Replace(stagingPath, destinationPath, destinationBackupFileName: null);
        else
            File.Move(stagingPath, destinationPath);
    }

    private static void DeleteExactStaging(string path)
    {
        try
        {
            if (File.Exists(path))
                File.Delete(path);
        }
        catch
        {
            // Preserve the publication failure; cleanup is restricted to this exact GUID path.
        }
    }

    private static string BuildLogEntry(
        GameParamSourceSet sources,
        IReadOnlyCollection<SourceHash> hashes,
        string cache,
        GameParamMergeResult? merge,
        string outputHash)
    {
        string sourceText = string.Join(" ", hashes.Select(source =>
            $"source[{source.Kind}]={JsonSerializer.Serialize(source.Path)} sha256={source.Sha256}"));
        string counts = merge is null
            ? "counts=reused"
            : $"changedEntries={merge.ChangedEntries} addedRows={merge.AddedRows} changedRows={merge.ChangedRows} " +
              $"deletedRows={merge.DeletedRows} preservedTargetRows={merge.PreservedTargetRows} " +
              $"randomizerWins={merge.RandomizerWinsOverlaps}";
        return $"gameparam status=success cache={cache} {sourceText} {counts} " +
               $"output={JsonSerializer.Serialize(sources.OutputPath)} sha256={outputHash}";
    }

    public sealed record SourceHash(string Kind, string Path, string Sha256);

    public sealed record MergeCountManifest(
        int ChangedEntries,
        int AddedRows,
        int ChangedRows,
        int DeletedRows,
        int PreservedTargetRows,
        int RandomizerWinsOverlaps);

    public sealed record MergeManifest(
        int SchemaVersion,
        string DependencyCommit,
        IReadOnlyList<SourceHash> Sources,
        string OutputSha256,
        MergeCountManifest Counts);
}
