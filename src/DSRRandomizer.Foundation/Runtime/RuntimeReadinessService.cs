using System.Text.Json;
using DSRRandomizer.Foundation.Paths;

namespace DSRRandomizer.Foundation.Runtime;

public sealed record RuntimeReadinessResult(
    bool IsReady,
    string? RuntimePath,
    IReadOnlyList<string> Errors);

public sealed class RuntimeReadinessService
{
    private const string ManifestFileName = "runtime-manifest.json";
    private readonly LocalDataLayout _layout;
    private readonly WriteBoundary _boundary;
    private readonly IPathCanonicalizer _canonicalizer;
    private readonly FileHashService _hashes;
    private readonly RuntimePointerStore _pointerStore;

    public RuntimeReadinessService(
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

            if (!manifest.Files.Any(file => file.RelativePath.Equals(
                    "DarkSoulsRemastered.exe",
                    StringComparison.OrdinalIgnoreCase)))
            {
                return NotReady("The runtime manifest does not contain DarkSoulsRemastered.exe.");
            }

            var expectedPaths = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
            {
                ManifestFileName
            };
            foreach (var file in manifest.Files)
            {
                if (!expectedPaths.Add(file.RelativePath))
                {
                    return NotReady($"The runtime manifest contains a duplicate path: {file.RelativePath}");
                }

                var filePath = RuntimePathSafety.ResolveUnderRoot(runtimePath, file.RelativePath);
                _boundary.EnsureAllowed(filePath);
                if (!File.Exists(filePath))
                {
                    return NotReady($"The runtime file is missing: {file.RelativePath}");
                }

                var info = new FileInfo(filePath);
                var hash = await _hashes.ComputeSha256Async(filePath, cancellationToken);
                if (info.Length != file.Length
                    || !hash.Equals(file.Sha256, StringComparison.OrdinalIgnoreCase))
                {
                    return NotReady($"Runtime file hash or length mismatch: {file.RelativePath}");
                }
            }

            var actualPaths = EnumerateRuntimeFiles(runtimePath, cancellationToken);
            var unexpected = actualPaths.FirstOrDefault(path => !expectedPaths.Contains(path));
            if (unexpected is not null)
            {
                return NotReady($"The runtime contains an unmanifested file: {unexpected}");
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

    private static IReadOnlyList<string> EnumerateRuntimeFiles(
        string runtimeRoot,
        CancellationToken cancellationToken)
    {
        var files = new List<string>();
        var pending = new Stack<string>();
        pending.Push(runtimeRoot);
        while (pending.TryPop(out var directory))
        {
            cancellationToken.ThrowIfCancellationRequested();
            foreach (var entry in new DirectoryInfo(directory).EnumerateFileSystemInfos())
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
                else
                {
                    files.Add(Path.GetRelativePath(runtimeRoot, entry.FullName)
                        .Replace(Path.DirectorySeparatorChar, '/'));
                }
            }
        }

        files.Sort(StringComparer.Ordinal);
        return files;
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
