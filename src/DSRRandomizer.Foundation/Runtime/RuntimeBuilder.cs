using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using DSRRandomizer.Foundation.Installation;
using DSRRandomizer.Foundation.Paths;

namespace DSRRandomizer.Foundation.Runtime;

public sealed class RuntimeBuilder
{
    private const long ReservedBytes = 536_870_912;
    private const string ManifestFileName = "runtime-manifest.json";

    private readonly LocalDataLayout _layout;
    private readonly WriteBoundary _boundary;
    private readonly IFileCopier _fileCopier;
    private readonly IDiskSpaceProbe _diskSpace;
    private readonly IClock _clock;
    private readonly FileHashService _hashes;
    private readonly RuntimePointerStore _pointerStore;

    public RuntimeBuilder(
        LocalDataLayout layout,
        WriteBoundary boundary,
        IFileCopier fileCopier,
        IDiskSpaceProbe diskSpace,
        IClock clock,
        FileHashService hashes,
        RuntimePointerStore pointerStore)
    {
        _layout = layout;
        _boundary = boundary;
        _fileCopier = fileCopier;
        _diskSpace = diskSpace;
        _clock = clock;
        _hashes = hashes;
        _pointerStore = pointerStore;
    }

    public async Task<RuntimeManifest> BuildAsync(
        string sourceRoot,
        GameFileCatalog catalog,
        IProgress<RuntimeBuildProgress>? progress,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(catalog);
        EnsureLayoutPathsAllowed();

        var requiredBytes = checked(catalog.TotalBytes + ReservedBytes);
        if (_diskSpace.GetAvailableBytes(_layout.Root) < requiredBytes)
        {
            throw new IOException(
                $"Insufficient free space. Runtime creation requires {requiredBytes} available bytes.");
        }

        Directory.CreateDirectory(_layout.Root);
        Directory.CreateDirectory(_layout.Runtimes);
        Directory.CreateDirectory(_layout.Staging);

        var stagingPath = Path.Combine(_layout.Staging, $"runtime-{Guid.NewGuid():N}");
        _boundary.EnsureAllowed(stagingPath);
        Directory.CreateDirectory(stagingPath);
        var ownsStaging = true;

        try
        {
            var before = await SourceSnapshot.CaptureAsync(
                sourceRoot,
                catalog,
                _hashes,
                cancellationToken);
            if (!before.MatchesCatalog(catalog))
            {
                throw new IOException("The source changed after installation verification.");
            }

            long copiedBytes = 0;
            foreach (var file in before.Files)
            {
                cancellationToken.ThrowIfCancellationRequested();
                var source = RuntimePathSafety.ResolveUnderRoot(sourceRoot, file.RelativePath);
                var destination = RuntimePathSafety.ResolveUnderRoot(stagingPath, file.RelativePath);
                _boundary.EnsureAllowed(destination);
                var parent = Path.GetDirectoryName(destination)
                    ?? throw new IOException($"Destination has no parent: {destination}");
                _boundary.EnsureAllowed(parent);
                Directory.CreateDirectory(parent);

                _fileCopier.Copy(source, destination);
                var destinationHash = await _hashes.ComputeSha256Async(destination, cancellationToken);
                if (!destinationHash.Equals(file.Sha256, StringComparison.OrdinalIgnoreCase))
                {
                    throw new IOException($"Destination hash mismatch: {file.RelativePath}");
                }

                copiedBytes = checked(copiedBytes + file.Length);
                progress?.Report(new RuntimeBuildProgress(
                    copiedBytes,
                    catalog.TotalBytes,
                    file.RelativePath));
            }

            var after = await SourceSnapshot.CaptureAsync(
                sourceRoot,
                catalog,
                _hashes,
                cancellationToken);
            if (!before.IsEquivalentTo(after))
            {
                throw new IOException("The source changed while the external runtime was being copied.");
            }

            var manifest = CreateManifest(catalog, before, stagingPath);
            var manifestPath = Path.Combine(stagingPath, ManifestFileName);
            _boundary.EnsureAllowed(manifestPath);
            await File.WriteAllBytesAsync(
                manifestPath,
                JsonSerializer.SerializeToUtf8Bytes(manifest, RuntimeJson.Options),
                cancellationToken);

            var finalPath = Path.Combine(_layout.Runtimes, manifest.RuntimeId);
            _boundary.EnsureAllowed(finalPath);
            if (Directory.Exists(finalPath))
            {
                Directory.Delete(stagingPath, recursive: true);
                ownsStaging = false;
                manifest = await ReadExistingManifestAsync(finalPath, manifest.RuntimeId, cancellationToken);
            }
            else
            {
                Directory.Move(stagingPath, finalPath);
                ownsStaging = false;
                manifest = manifest with { RuntimePath = finalPath };
            }

            var finalManifestPath = Path.Combine(finalPath, ManifestFileName);
            var manifestSha256 = await _hashes.ComputeSha256Async(
                finalManifestPath,
                cancellationToken);
            await _pointerStore.ActivateAsync(
                new RuntimePointer(
                    manifest.RuntimeId,
                    $"runtimes/{manifest.RuntimeId}",
                    manifestSha256),
                cancellationToken);
            var modsPath = Path.Combine(finalPath, LocalDataLayout.ModsDirectoryName);
            _boundary.EnsureAllowed(modsPath);
            Directory.CreateDirectory(modsPath);
            return manifest;
        }
        catch
        {
            if (ownsStaging && Directory.Exists(stagingPath))
            {
                _boundary.EnsureAllowed(stagingPath);
                Directory.Delete(stagingPath, recursive: true);
            }

            throw;
        }
    }

    private RuntimeManifest CreateManifest(
        GameFileCatalog catalog,
        SourceSnapshot snapshot,
        string stagingPath)
    {
        var files = snapshot.Files
            .Select(file => new RuntimeFileManifestEntry(
                file.RelativePath,
                file.Length,
                file.Sha256))
            .ToArray();
        var catalogHash = ComputeCatalogHash(catalog);
        var sourceExecutableHash = snapshot
            .GetRequired("DarkSoulsRemastered.exe")
            .Sha256;
        var runtimeId = "runtime-" + ComputeContentDescriptorHash(
            sourceExecutableHash,
            catalogHash,
            catalog.TotalBytes,
            files);
        return new RuntimeManifest(
            1,
            runtimeId,
            _clock.UtcNow,
            sourceExecutableHash,
            catalogHash,
            catalog.TotalBytes,
            files)
        {
            RuntimePath = stagingPath
        };
    }

    private async Task<RuntimeManifest> ReadExistingManifestAsync(
        string runtimePath,
        string expectedRuntimeId,
        CancellationToken cancellationToken)
    {
        var manifestPath = Path.Combine(runtimePath, ManifestFileName);
        await using var stream = File.OpenRead(manifestPath);
        var manifest = await JsonSerializer.DeserializeAsync<RuntimeManifest>(
            stream,
            RuntimeJson.Options,
            cancellationToken)
            ?? throw new IOException($"Existing runtime manifest is invalid: {manifestPath}");
        if (!manifest.RuntimeId.Equals(expectedRuntimeId, StringComparison.Ordinal))
        {
            throw new IOException($"Existing runtime ID does not match its directory: {runtimePath}");
        }

        return manifest with { RuntimePath = runtimePath };
    }

    private void EnsureLayoutPathsAllowed()
    {
        _boundary.EnsureAllowed(_layout.Root);
        _boundary.EnsureAllowed(_layout.Runtimes);
        _boundary.EnsureAllowed(_layout.Staging);
    }

    private static string ComputeCatalogHash(GameFileCatalog catalog)
    {
        var text = string.Join(
            '\n',
            catalog.Files
                .OrderBy(entry => entry.RelativePath, StringComparer.Ordinal)
                .Select(entry => $"{entry.RelativePath}\0{entry.Length}"));
        return HashText(text);
    }

    private static string ComputeContentDescriptorHash(
        string sourceExecutableHash,
        string catalogHash,
        long totalBytes,
        IReadOnlyList<RuntimeFileManifestEntry> files)
    {
        var builder = new StringBuilder()
            .Append("schemaVersion=1\n")
            .Append("sourceExecutableSha256=").Append(sourceExecutableHash).Append('\n')
            .Append("catalogSha256=").Append(catalogHash).Append('\n')
            .Append("totalBytes=").Append(totalBytes).Append('\n');
        foreach (var file in files.OrderBy(entry => entry.RelativePath, StringComparer.Ordinal))
        {
            builder.Append(file.RelativePath)
                .Append('\0')
                .Append(file.Length)
                .Append('\0')
                .Append(file.Sha256)
                .Append('\n');
        }

        return HashText(builder.ToString());
    }

    private static string HashText(string text) =>
        Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(text))).ToLowerInvariant();
}
