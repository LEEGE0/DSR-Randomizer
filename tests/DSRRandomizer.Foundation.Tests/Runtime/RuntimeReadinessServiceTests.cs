using System.Text.Json;
using DSRRandomizer.Foundation.Paths;
using DSRRandomizer.Foundation.Runtime;

namespace DSRRandomizer.Foundation.Tests.Runtime;

public sealed class RuntimeReadinessServiceTests
{
    [Fact]
    public async Task ValidateAsync_AcceptsOnlyCompleteManifestVerifiedExternalRuntime()
    {
        using var fixture = RuntimeReadinessFixture.Create();
        await fixture.CreateValidRuntimeAsync();

        var result = await fixture.Service.ValidateAsync(CancellationToken.None);

        Assert.True(result.IsReady, string.Join(Environment.NewLine, result.Errors));
        Assert.Equal(fixture.RuntimeRoot, result.RuntimePath);
        Assert.DoesNotContain(fixture.SourceRoot, result.RuntimePath!, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public async Task ValidateAsync_RejectsMissingPointer()
    {
        using var fixture = RuntimeReadinessFixture.Create();

        var result = await fixture.Service.ValidateAsync(CancellationToken.None);

        Assert.False(result.IsReady);
        Assert.Contains(result.Errors, error => error.Contains("pointer", StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public async Task ValidateAsync_RejectsManifestHashMismatch()
    {
        using var fixture = RuntimeReadinessFixture.Create();
        await fixture.CreateValidRuntimeAsync();
        await File.AppendAllTextAsync(fixture.ManifestPath, " ");

        var result = await fixture.Service.ValidateAsync(CancellationToken.None);

        Assert.False(result.IsReady);
        Assert.Contains(result.Errors, error => error.Contains("manifest hash", StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public async Task ValidateAsync_RejectsCopiedFileHashMismatch()
    {
        using var fixture = RuntimeReadinessFixture.Create();
        await fixture.CreateValidRuntimeAsync();
        await File.AppendAllTextAsync(Path.Combine(fixture.RuntimeRoot, "map", "test.dcx"), "corrupt");

        var result = await fixture.Service.ValidateAsync(CancellationToken.None);

        Assert.False(result.IsReady);
        Assert.Contains(result.Errors, error => error.Contains("file hash", StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public async Task ValidateAsync_RejectsMissingCopiedExecutable()
    {
        using var fixture = RuntimeReadinessFixture.Create();
        await fixture.CreateValidRuntimeAsync();
        File.Delete(Path.Combine(fixture.RuntimeRoot, "DarkSoulsRemastered.exe"));

        var result = await fixture.Service.ValidateAsync(CancellationToken.None);

        Assert.False(result.IsReady);
        Assert.Contains(result.Errors, error => error.Contains("DarkSoulsRemastered.exe", StringComparison.Ordinal));
    }

    [Fact]
    public async Task ValidateAsync_RejectsPointerThatEscapesTowardSourceInstallation()
    {
        using var fixture = RuntimeReadinessFixture.Create();
        var maliciousPointer = new RuntimePointer("runtime-escape", "../source/runtime-escape", "hash");
        await File.WriteAllTextAsync(
            Path.Combine(fixture.Layout.Root, "runtime-current.json"),
            JsonSerializer.Serialize(maliciousPointer, RuntimeReadinessFixture.JsonOptions));

        var result = await fixture.Service.ValidateAsync(CancellationToken.None);

        Assert.False(result.IsReady);
        Assert.Contains(result.Errors, error => error.Contains("root", StringComparison.OrdinalIgnoreCase));
    }

    private sealed class RuntimeReadinessFixture : IDisposable
    {
        private readonly string _container;
        private readonly FileHashService _hashes = new();
        private readonly RuntimePointerStore _pointerStore;

        private RuntimeReadinessFixture(string container)
        {
            _container = container;
            SourceRoot = Path.Combine(container, "source");
            var localRoot = Path.Combine(container, "local");
            Directory.CreateDirectory(SourceRoot);
            Directory.CreateDirectory(localRoot);
            var canonicalizer = new WindowsPathCanonicalizer();
            var boundary = WriteBoundary.Create(SourceRoot, localRoot, canonicalizer);
            Layout = LocalDataLayout.Create(localRoot, boundary);
            Directory.CreateDirectory(Layout.Root);
            Directory.CreateDirectory(Layout.Runtimes);
            RuntimeRoot = Path.Combine(Layout.Runtimes, "runtime-abc");
            ManifestPath = Path.Combine(RuntimeRoot, "runtime-manifest.json");
            _pointerStore = new RuntimePointerStore(Layout, boundary);
            Service = new RuntimeReadinessService(
                Layout,
                boundary,
                canonicalizer,
                _hashes,
                _pointerStore);
        }

        public static JsonSerializerOptions JsonOptions { get; } = new()
        {
            PropertyNamingPolicy = JsonNamingPolicy.CamelCase
        };

        public string SourceRoot { get; }

        public LocalDataLayout Layout { get; }

        public string RuntimeRoot { get; }

        public string ManifestPath { get; }

        public RuntimeReadinessService Service { get; }

        public static RuntimeReadinessFixture Create()
        {
            var container = Path.Combine(Path.GetTempPath(), $"dsr-readiness-{Guid.NewGuid():N}");
            Directory.CreateDirectory(container);
            return new RuntimeReadinessFixture(container);
        }

        public async Task CreateValidRuntimeAsync()
        {
            Directory.CreateDirectory(Path.Combine(RuntimeRoot, "map"));
            var executablePath = Path.Combine(RuntimeRoot, "DarkSoulsRemastered.exe");
            var mapPath = Path.Combine(RuntimeRoot, "map", "test.dcx");
            await File.WriteAllTextAsync(executablePath, "game");
            await File.WriteAllTextAsync(mapPath, "map");
            var files = new[]
            {
                new RuntimeFileManifestEntry(
                    "DarkSoulsRemastered.exe",
                    new FileInfo(executablePath).Length,
                    await _hashes.ComputeSha256Async(executablePath, CancellationToken.None)),
                new RuntimeFileManifestEntry(
                    "map/test.dcx",
                    new FileInfo(mapPath).Length,
                    await _hashes.ComputeSha256Async(mapPath, CancellationToken.None))
            };
            var manifest = new RuntimeManifest(
                1,
                "runtime-abc",
                new DateTimeOffset(2026, 8, 24, 0, 0, 0, TimeSpan.Zero),
                files[0].Sha256,
                "catalog-hash",
                files.Sum(file => file.Length),
                files);
            await File.WriteAllTextAsync(ManifestPath, JsonSerializer.Serialize(manifest, JsonOptions));
            await _pointerStore.ActivateAsync(
                new RuntimePointer(
                    "runtime-abc",
                    "runtimes/runtime-abc",
                    await _hashes.ComputeSha256Async(ManifestPath, CancellationToken.None)),
                CancellationToken.None);
        }

        public void Dispose() => Directory.Delete(_container, recursive: true);
    }
}
