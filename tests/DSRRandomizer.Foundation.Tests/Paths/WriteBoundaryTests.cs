using DSRRandomizer.Foundation.Paths;

namespace DSRRandomizer.Foundation.Tests.Paths;

public sealed class WriteBoundaryTests
{
    [Fact]
    public void EnsureAllowed_RejectsSourceDescendantAndResolvedAlias()
    {
        var canonicalizer = new FakeCanonicalizer(new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            [@"C:\Steam\DSR"] = @"C:\Steam\DSR",
            [@"C:\Steam\DSR\param\file.dcx"] = @"C:\Steam\DSR\param\file.dcx",
            [@"C:\Local\DSR"] = @"C:\Local\DSR",
            [@"C:\Local\DSR\escape\file.dcx"] = @"C:\Steam\DSR\param\file.dcx"
        });
        var boundary = WriteBoundary.Create(@"C:\Steam\DSR", @"C:\Local\DSR", canonicalizer);

        Assert.Throws<UnauthorizedAccessException>(
            () => boundary.EnsureAllowed(@"C:\Steam\DSR\param\file.dcx"));
        Assert.Throws<UnauthorizedAccessException>(
            () => boundary.EnsureAllowed(@"C:\Local\DSR\escape\file.dcx"));
    }

    [Fact]
    public void EnsureAllowed_RejectsEveryPathOutsideLocalRoot()
    {
        var boundary = WriteBoundary.Create(
            @"C:\Steam\DSR",
            @"C:\Local\DSR",
            new IdentityCanonicalizer());

        boundary.EnsureAllowed(@"C:\Local\DSR\staging\runtime.json");

        Assert.Throws<UnauthorizedAccessException>(
            () => boundary.EnsureAllowed(@"C:\Other\runtime.json"));
        Assert.Throws<UnauthorizedAccessException>(
            () => boundary.EnsureAllowed(@"C:\Local\DSR-escape\runtime.json"));
    }

    [Theory]
    [InlineData(@"C:\Steam\DSR", @"C:\Steam\DSR")]
    [InlineData(@"C:\Steam\DSR", @"C:\Steam\DSR\randomizer")]
    [InlineData(@"C:\Local", @"C:\Local\DSR\source")]
    public void Create_RejectsEqualOrOverlappingRoots(string sourceRoot, string localRoot)
    {
        Assert.Throws<ArgumentException>(
            () => WriteBoundary.Create(sourceRoot, localRoot, new IdentityCanonicalizer()));
    }

    [Fact]
    public void LocalDataLayout_ProducesOnlyAllowedDescendants()
    {
        var boundary = WriteBoundary.Create(
            @"C:\Steam\DSR",
            @"C:\Local\DSR",
            new IdentityCanonicalizer());

        var layout = LocalDataLayout.Create(@"C:\Local\DSR", boundary);

        var expected = new[]
        {
            @"C:\Local\DSR",
            @"C:\Local\DSR\runtimes",
            @"C:\Local\DSR\staging",
            @"C:\Local\DSR\active-seed",
            @"C:\Local\DSR\saves",
            @"C:\Local\DSR\config",
            @"C:\Local\DSR\logs"
        };
        Assert.Equal(expected, new[]
        {
            layout.Root,
            layout.Runtimes,
            layout.Staging,
            layout.ActiveSeed,
            layout.Saves,
            layout.Config,
            layout.Logs
        });
    }

    private sealed class IdentityCanonicalizer : IPathCanonicalizer
    {
        public string Canonicalize(string path) => Path.GetFullPath(path).TrimEnd(Path.DirectorySeparatorChar);
    }

    private sealed class FakeCanonicalizer(IReadOnlyDictionary<string, string> mappings) : IPathCanonicalizer
    {
        public string Canonicalize(string path) => mappings.TryGetValue(path, out var mapped)
            ? mapped
            : Path.GetFullPath(path).TrimEnd(Path.DirectorySeparatorChar);
    }
}

public sealed class WindowsPathCanonicalizerTests : IDisposable
{
    private readonly string _temporaryRoot = Path.Combine(
        Path.GetTempPath(),
        $"dsr-randomizer-paths-{Guid.NewGuid():N}");

    public WindowsPathCanonicalizerTests() => Directory.CreateDirectory(_temporaryRoot);

    [Fact]
    public void Canonicalize_ResolvesExistingAncestorAndAppendsMissingSegments()
    {
        var canonicalizer = new WindowsPathCanonicalizer();

        var result = canonicalizer.Canonicalize(Path.Combine(_temporaryRoot, "new", "file.dcx"));

        Assert.Equal(Path.Combine(_temporaryRoot, "new", "file.dcx"), result, ignoreCase: true);
        Assert.False(result.StartsWith(@"\\?\", StringComparison.Ordinal));
    }

    [Theory]
    [InlineData(@"\\?\C:\Games\DSR", @"C:\Games\DSR")]
    [InlineData(@"\\?\UNC\server\share\DSR", @"\\server\share\DSR")]
    public void NormalizeFinalPath_RemovesWindowsDevicePrefixes(string finalPath, string expected)
    {
        Assert.Equal(expected, WindowsPathCanonicalizer.NormalizeFinalPath(finalPath));
    }

    [Fact]
    public void Canonicalize_FailsClosedWhenNoExistingAncestorCanBeResolved()
    {
        var canonicalizer = new WindowsPathCanonicalizer();

        Assert.Throws<IOException>(() => canonicalizer.Canonicalize(@"Z:\missing\path"));
    }

    public void Dispose() => Directory.Delete(_temporaryRoot, recursive: true);
}
