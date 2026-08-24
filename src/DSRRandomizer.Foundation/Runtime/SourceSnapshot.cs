using DSRRandomizer.Foundation.Installation;

namespace DSRRandomizer.Foundation.Runtime;

public sealed record SourceSnapshotEntry(
    string RelativePath,
    long Length,
    DateTime LastWriteTimeUtc,
    string Sha256);

public sealed class SourceSnapshot
{
    public SourceSnapshot(IReadOnlyList<SourceSnapshotEntry> files) => Files = files;

    public IReadOnlyList<SourceSnapshotEntry> Files { get; }

    public static async Task<SourceSnapshot> CaptureAsync(
        string sourceRoot,
        GameFileCatalog catalog,
        FileHashService hashes,
        CancellationToken cancellationToken)
    {
        var entries = new List<SourceSnapshotEntry>(catalog.Files.Count);
        foreach (var catalogEntry in catalog.Files.OrderBy(
                     entry => entry.RelativePath,
                     StringComparer.Ordinal))
        {
            cancellationToken.ThrowIfCancellationRequested();
            var path = RuntimePathSafety.ResolveUnderRoot(sourceRoot, catalogEntry.RelativePath);
            var info = new FileInfo(path);
            if (!info.Exists)
            {
                throw new IOException($"Source file disappeared: {catalogEntry.RelativePath}");
            }

            entries.Add(new SourceSnapshotEntry(
                catalogEntry.RelativePath,
                info.Length,
                info.LastWriteTimeUtc,
                await hashes.ComputeSha256Async(path, cancellationToken)));
        }

        return new SourceSnapshot(entries);
    }

    public SourceSnapshotEntry GetRequired(string relativePath) =>
        Files.Single(entry => entry.RelativePath.Equals(relativePath, StringComparison.OrdinalIgnoreCase));

    public bool IsEquivalentTo(SourceSnapshot other) =>
        Files.Count == other.Files.Count
        && Files.Zip(other.Files).All(pair => pair.First == pair.Second);

    public bool MatchesCatalog(GameFileCatalog catalog) =>
        Files.Count == catalog.Files.Count
        && Files.Zip(catalog.Files.OrderBy(entry => entry.RelativePath, StringComparer.Ordinal))
            .All(pair =>
                pair.First.RelativePath.Equals(pair.Second.RelativePath, StringComparison.Ordinal)
                && pair.First.Length == pair.Second.Length
                && pair.First.LastWriteTimeUtc == pair.Second.LastWriteTimeUtc);
}
