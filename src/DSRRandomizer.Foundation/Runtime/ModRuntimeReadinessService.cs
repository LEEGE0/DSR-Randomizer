using System.Text.Json;
using DSRRandomizer.Foundation.Paths;

namespace DSRRandomizer.Foundation.Runtime;

public sealed class ModRuntimeReadinessService
{
    private const string ManifestFileName = "runtime-manifest.json";
    private static readonly string[] ProtectedCore =
    [
        "DarkSoulsRemastered.exe",
        "steam_api64.dll"
    ];

    private readonly LocalDataLayout _layout;
    private readonly WriteBoundary _boundary;
    private readonly IPathCanonicalizer _canonicalizer;
    private readonly FileHashService _hashes;
    private readonly RuntimePointerStore _pointerStore;

    public ModRuntimeReadinessService(
        LocalDataLayout layout,
        WriteBoundary boundary,
        IPathCanonicalizer canonicalizer,
        FileHashService hashes,
        RuntimePointerStore pointerStore)
    {
        _layout = layout;
        _boundary = boundary;
        _canonicalizer = canonicalizer;
        _hashes = hashes;
        _pointerStore = pointerStore;
    }

    public async Task<RuntimeReadinessResult> ValidateAsync(
        CancellationToken cancellationToken)
    {
        try
        {
            var pointer = await _pointerStore.ReadAsync(cancellationToken);
            if (pointer is null)
            {
                return NotReady("The current runtime pointer does not exist.");
            }

            var runtimeCandidate = RuntimePathSafety.ResolveUnderRoot(
                _layout.Root,
                pointer.RelativeRuntimePath);
            _boundary.EnsureAllowed(runtimeCandidate);
            var runtimePath = _canonicalizer.Canonicalize(runtimeCandidate);
            var runtimesRoot = _canonicalizer.Canonicalize(_layout.Runtimes);
            if (!IsStrictDescendant(runtimePath, runtimesRoot))
            {
                return NotReady("The runtime path is outside the local runtimes root.");
            }

            if (!Directory.Exists(runtimePath))
            {
                return NotReady($"The pointed runtime directory is missing: {runtimePath}");
            }

            if (!Path.GetFileName(runtimePath).Equals(pointer.RuntimeId, StringComparison.Ordinal))
            {
                return NotReady("The runtime pointer ID does not match its directory.");
            }

            EnsureNoReparsePoints(runtimePath, cancellationToken);

            var manifestPath = Path.Combine(runtimePath, ManifestFileName);
            if (!File.Exists(manifestPath))
            {
                return NotReady($"The runtime manifest is missing: {manifestPath}");
            }

            var manifestHash = await _hashes.ComputeSha256Async(manifestPath, cancellationToken);
            if (!manifestHash.Equals(pointer.ManifestSha256, StringComparison.OrdinalIgnoreCase))
            {
                return NotReady("The runtime manifest hash does not match the active pointer.");
            }

            await using var manifestStream = File.OpenRead(manifestPath);
            var manifest = await JsonSerializer.DeserializeAsync<RuntimeManifest>(
                manifestStream,
                RuntimeJson.Options,
                cancellationToken);
            if (manifest is null || manifest.SchemaVersion != 1)
            {
                return NotReady("The runtime manifest is invalid or unsupported.");
            }

            if (!manifest.RuntimeId.Equals(pointer.RuntimeId, StringComparison.Ordinal))
            {
                return NotReady("The runtime manifest ID does not match the active pointer.");
            }

            var manifestEntries = new Dictionary<string, RuntimeFileManifestEntry>(
                StringComparer.OrdinalIgnoreCase);
            foreach (var file in manifest.Files)
            {
                if (!manifestEntries.TryAdd(file.RelativePath, file))
                {
                    return NotReady($"The runtime manifest contains a duplicate path: {file.RelativePath}");
                }

                _ = RuntimePathSafety.ResolveUnderRoot(runtimePath, file.RelativePath);
            }

            foreach (var relativePath in ProtectedCore)
            {
                if (!manifestEntries.TryGetValue(relativePath, out var entry))
                {
                    return NotReady($"The runtime manifest does not contain {relativePath}.");
                }

                var filePath = RuntimePathSafety.ResolveUnderRoot(runtimePath, entry.RelativePath);
                _boundary.EnsureAllowed(filePath);
                if (!File.Exists(filePath))
                {
                    return NotReady($"The protected runtime file is missing: {relativePath}");
                }

                var info = new FileInfo(filePath);
                var hash = await _hashes.ComputeSha256Async(filePath, cancellationToken);
                if (info.Length != entry.Length
                    || !hash.Equals(entry.Sha256, StringComparison.OrdinalIgnoreCase))
                {
                    return NotReady($"Protected runtime file hash or length mismatch: {relativePath}");
                }
            }

            return new RuntimeReadinessResult(true, runtimePath, Array.Empty<string>());
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (Exception exception) when (
            exception is IOException
                or UnauthorizedAccessException
                or ArgumentException
                or JsonException)
        {
            return NotReady(exception.Message);
        }
    }

    private static void EnsureNoReparsePoints(
        string runtimeRoot,
        CancellationToken cancellationToken)
    {
        var pending = new Stack<string>();
        pending.Push(runtimeRoot);
        while (pending.TryPop(out var directory))
        {
            cancellationToken.ThrowIfCancellationRequested();
            var directoryInfo = new DirectoryInfo(directory);
            if ((directoryInfo.Attributes & FileAttributes.ReparsePoint) != 0)
            {
                throw new IOException($"Reparse points are not allowed in a runtime: {directoryInfo.FullName}");
            }

            foreach (var entry in directoryInfo.EnumerateFileSystemInfos())
            {
                cancellationToken.ThrowIfCancellationRequested();
                if ((entry.Attributes & FileAttributes.ReparsePoint) != 0)
                {
                    throw new IOException($"Reparse points are not allowed in a runtime: {entry.FullName}");
                }

                if ((entry.Attributes & FileAttributes.Directory) != 0)
                {
                    pending.Push(entry.FullName);
                }
            }
        }
    }

    private static bool IsStrictDescendant(string candidate, string root)
    {
        var normalizedRoot = Path.TrimEndingDirectorySeparator(root);
        return candidate.StartsWith(
            normalizedRoot + Path.DirectorySeparatorChar,
            StringComparison.OrdinalIgnoreCase);
    }

    private static RuntimeReadinessResult NotReady(string error) =>
        new(false, null, new[] { error });
}
