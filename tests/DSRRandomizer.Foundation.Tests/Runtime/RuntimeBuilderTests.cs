using System.Security.Cryptography;
using DSRRandomizer.Foundation.Installation;
using DSRRandomizer.Foundation.Paths;
using DSRRandomizer.Foundation.Runtime;

namespace DSRRandomizer.Foundation.Tests.Runtime;

public sealed class RuntimeBuilderTests
{
    [Fact]
    public async Task BuildAsync_CopiesIndependentFilesAndLeavesSourceUnchanged()
    {
        using var fixture = RuntimeBuilderFixture.Create();
        var before = TestSnapshot.Capture(fixture.SourceRoot);

        var manifest = await fixture.Builder.BuildAsync(
            fixture.SourceRoot,
            fixture.Catalog,
            progress: null,
            CancellationToken.None);

        var copiedExe = Path.Combine(manifest.RuntimePath, "DarkSoulsRemastered.exe");
        Assert.Equal("game", File.ReadAllText(copiedExe));
        File.AppendAllText(copiedExe, "-changed-copy");
        Assert.Equal("game", File.ReadAllText(fixture.SourcePath("DarkSoulsRemastered.exe")));
        Assert.Equal(before, TestSnapshot.Capture(fixture.SourceRoot));

        var pointer = await fixture.PointerStore.ReadAsync(CancellationToken.None);
        Assert.NotNull(pointer);
        Assert.Equal(manifest.RuntimeId, pointer.RuntimeId);
        Assert.Equal(manifest.RuntimeId, Path.GetFileName(manifest.RuntimePath));
    }

    [Fact]
    public async Task BuildAsync_CopiesOnlyVerifiedCatalogEntries()
    {
        using var fixture = RuntimeBuilderFixture.Create();
        fixture.WriteUnlistedSource("map/MapStudio/unlisted-mod-file.dcx", "not in catalog");

        var manifest = await fixture.Builder.BuildAsync(
            fixture.SourceRoot,
            fixture.Catalog,
            progress: null,
            CancellationToken.None);

        Assert.False(File.Exists(Path.Combine(
            manifest.RuntimePath,
            "map",
            "MapStudio",
            "unlisted-mod-file.dcx")));
        Assert.DoesNotContain(manifest.Files, entry => entry.RelativePath.Equals(
            "map/MapStudio/unlisted-mod-file.dcx",
            StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public async Task BuildAsync_CopyFailurePreservesCurrentPointerAndExistingRuntime()
    {
        using var fixture = RuntimeBuilderFixture.Create(
            copier: new ThrowingFileCopier("m10_00_00_00.msb.dcx"));
        await fixture.SetActiveRuntimeAsync("runtime-old");

        await Assert.ThrowsAsync<IOException>(() => fixture.Builder.BuildAsync(
            fixture.SourceRoot,
            fixture.Catalog,
            null,
            CancellationToken.None));

        Assert.Equal(
            "runtime-old",
            (await fixture.PointerStore.ReadAsync(CancellationToken.None))!.RuntimeId);
        Assert.True(Directory.Exists(fixture.RuntimePath("runtime-old")));
        Assert.Empty(Directory.EnumerateDirectories(fixture.Layout.Staging));
    }

    [Fact]
    public async Task BuildAsync_InsufficientDiskSpaceDoesNotCreateStagingOrChangePointer()
    {
        using var fixture = RuntimeBuilderFixture.Create(availableBytes: 1);
        await fixture.SetActiveRuntimeAsync("runtime-old");

        var exception = await Assert.ThrowsAsync<IOException>(() => fixture.Builder.BuildAsync(
            fixture.SourceRoot,
            fixture.Catalog,
            null,
            CancellationToken.None));

        Assert.Contains("free space", exception.Message, StringComparison.OrdinalIgnoreCase);
        Assert.Equal(
            "runtime-old",
            (await fixture.PointerStore.ReadAsync(CancellationToken.None))!.RuntimeId);
        Assert.Empty(Directory.EnumerateDirectories(fixture.Layout.Staging));
    }

    [Fact]
    public async Task BuildAsync_SourceMutationDuringCopyIsRejectedAndRolledBack()
    {
        using var fixture = RuntimeBuilderFixture.Create(copierFactory: sourceRoot =>
            new SourceMutatingFileCopier(Path.Combine(sourceRoot, "DarkSoulsRemastered.exe")));

        var exception = await Assert.ThrowsAsync<IOException>(() => fixture.Builder.BuildAsync(
            fixture.SourceRoot,
            fixture.Catalog,
            null,
            CancellationToken.None));

        Assert.Contains("source changed", exception.Message, StringComparison.OrdinalIgnoreCase);
        Assert.Null(await fixture.PointerStore.ReadAsync(CancellationToken.None));
        Assert.Empty(Directory.EnumerateDirectories(fixture.Layout.Staging));
    }

    [Fact]
    public async Task BuildAsync_DestinationHashMismatchIsRejectedAndRolledBack()
    {
        using var fixture = RuntimeBuilderFixture.Create(copier: new CorruptingFileCopier());

        var exception = await Assert.ThrowsAsync<IOException>(() => fixture.Builder.BuildAsync(
            fixture.SourceRoot,
            fixture.Catalog,
            null,
            CancellationToken.None));

        Assert.Contains("hash mismatch", exception.Message, StringComparison.OrdinalIgnoreCase);
        Assert.Null(await fixture.PointerStore.ReadAsync(CancellationToken.None));
        Assert.Empty(Directory.EnumerateDirectories(fixture.Layout.Staging));
    }

    [Fact]
    public async Task BuildAsync_DeniedStagingPathFailsBeforeWritingOutsideLocalRoot()
    {
        using var fixture = RuntimeBuilderFixture.Create();
        var deniedRoot = Path.Combine(fixture.Container, "denied");
        var unsafeLayout = fixture.Layout with { Staging = deniedRoot };
        var builder = fixture.CreateBuilder(unsafeLayout, new SystemFileCopier());

        await Assert.ThrowsAsync<UnauthorizedAccessException>(() => builder.BuildAsync(
            fixture.SourceRoot,
            fixture.Catalog,
            null,
            CancellationToken.None));

        Assert.False(Directory.Exists(deniedRoot));
    }

    private sealed class RuntimeBuilderFixture : IDisposable
    {
        private readonly IFileCopier _copier;
        private readonly long _availableBytes;

        private RuntimeBuilderFixture(string container, IFileCopier copier, long availableBytes)
        {
            Container = container;
            SourceRoot = Path.Combine(container, "source");
            var localRoot = Path.Combine(container, "local");
            Directory.CreateDirectory(SourceRoot);
            Directory.CreateDirectory(localRoot);
            WriteSource("DarkSoulsRemastered.exe", "game");
            WriteSource("map/MapStudio/m10_00_00_00.msb.dcx", "map");

            var canonicalizer = new WindowsPathCanonicalizer();
            Boundary = WriteBoundary.Create(SourceRoot, localRoot, canonicalizer);
            Layout = LocalDataLayout.Create(localRoot, Boundary);
            Directory.CreateDirectory(Layout.Root);
            Directory.CreateDirectory(Layout.Runtimes);
            Directory.CreateDirectory(Layout.Staging);
            PointerStore = new RuntimePointerStore(Layout, Boundary);
            Catalog = CreateCatalog(SourceRoot);
            _copier = copier;
            _availableBytes = availableBytes;
            Builder = CreateBuilder(Layout, copier);
        }

        public string Container { get; }

        public string SourceRoot { get; }

        public WriteBoundary Boundary { get; }

        public LocalDataLayout Layout { get; }

        public RuntimePointerStore PointerStore { get; }

        public GameFileCatalog Catalog { get; }

        public RuntimeBuilder Builder { get; }

        public static RuntimeBuilderFixture Create(
            IFileCopier? copier = null,
            Func<string, IFileCopier>? copierFactory = null,
            long availableBytes = long.MaxValue)
        {
            var container = Path.Combine(Path.GetTempPath(), $"dsr-runtime-{Guid.NewGuid():N}");
            Directory.CreateDirectory(container);
            var selectedCopier = copier ?? copierFactory?.Invoke(Path.Combine(container, "source")) ?? new SystemFileCopier();
            return new RuntimeBuilderFixture(container, selectedCopier, availableBytes);
        }

        public RuntimeBuilder CreateBuilder(LocalDataLayout layout, IFileCopier copier) =>
            new(
                layout,
                Boundary,
                copier,
                new FixedDiskSpaceProbe(_availableBytes),
                new FixedClock(new DateTimeOffset(2026, 8, 24, 0, 0, 0, TimeSpan.Zero)),
                new FileHashService(),
                PointerStore);

        public string SourcePath(string relativePath) =>
            Path.Combine(SourceRoot, relativePath.Replace('/', Path.DirectorySeparatorChar));

        public string RuntimePath(string runtimeId) => Path.Combine(Layout.Runtimes, runtimeId);

        public void WriteUnlistedSource(string relativePath, string content) =>
            WriteSource(relativePath, content);

        public async Task SetActiveRuntimeAsync(string runtimeId)
        {
            Directory.CreateDirectory(RuntimePath(runtimeId));
            await PointerStore.ActivateAsync(
                new RuntimePointer(runtimeId, $"runtimes/{runtimeId}", "old-manifest-hash"),
                CancellationToken.None);
        }

        public void Dispose() => Directory.Delete(Container, recursive: true);

        private void WriteSource(string relativePath, string content)
        {
            var path = SourcePath(relativePath);
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, content);
        }

        private static GameFileCatalog CreateCatalog(string root)
        {
            var paths = new[]
            {
                "DarkSoulsRemastered.exe",
                "map/MapStudio/m10_00_00_00.msb.dcx"
            };
            var entries = paths.Select(relativePath =>
            {
                var info = new FileInfo(Path.Combine(root, relativePath.Replace('/', Path.DirectorySeparatorChar)));
                return new GameFileEntry(relativePath, info.Length, info.LastWriteTimeUtc);
            }).ToArray();
            return new GameFileCatalog(entries, entries.Sum(entry => entry.Length));
        }
    }

    private sealed class FixedDiskSpaceProbe(long availableBytes) : IDiskSpaceProbe
    {
        public long GetAvailableBytes(string path) => availableBytes;
    }

    private sealed class FixedClock(DateTimeOffset utcNow) : IClock
    {
        public DateTimeOffset UtcNow => utcNow;
    }

    private sealed class ThrowingFileCopier(string fileName) : IFileCopier
    {
        public void Copy(string source, string destination)
        {
            if (source.EndsWith(fileName, StringComparison.OrdinalIgnoreCase))
            {
                throw new IOException("planned copy failure");
            }

            File.Copy(source, destination, overwrite: false);
        }
    }

    private sealed class CorruptingFileCopier : IFileCopier
    {
        public void Copy(string source, string destination) => File.WriteAllText(destination, "corrupt");
    }

    private sealed class SourceMutatingFileCopier(string sourceToMutate) : IFileCopier
    {
        private bool _mutated;

        public void Copy(string source, string destination)
        {
            File.Copy(source, destination, overwrite: false);
            if (!_mutated)
            {
                File.AppendAllText(sourceToMutate, "-mutated");
                _mutated = true;
            }
        }
    }

    private sealed class TestSnapshot : IEquatable<TestSnapshot>
    {
        private TestSnapshot(IReadOnlyList<string> entries) => Entries = entries;

        public IReadOnlyList<string> Entries { get; }

        public static TestSnapshot Capture(string root) =>
            new(Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories)
                .Order(StringComparer.Ordinal)
                .Select(path =>
                {
                    var info = new FileInfo(path);
                    var hash = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path)));
                    return $"{Path.GetRelativePath(root, path)}|{info.Length}|{info.LastWriteTimeUtc.Ticks}|{hash}";
                })
                .ToArray());

        public bool Equals(TestSnapshot? other) =>
            other is not null && Entries.SequenceEqual(other.Entries, StringComparer.Ordinal);

        public override bool Equals(object? obj) => Equals(obj as TestSnapshot);

        public override int GetHashCode() => Entries.Aggregate(17, HashCode.Combine);
    }
}
