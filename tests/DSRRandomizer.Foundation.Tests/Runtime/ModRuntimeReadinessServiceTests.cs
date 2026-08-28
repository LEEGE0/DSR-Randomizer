using System.Diagnostics;
using System.Text.Json;
using DSRRandomizer.Foundation.Paths;
using DSRRandomizer.Foundation.Runtime;

namespace DSRRandomizer.Foundation.Tests.Runtime;

public sealed class ModRuntimeReadinessServiceTests
{
    [Fact]
    public async Task ValidateAsync_AllowsOrdinaryDataReplacementAndNewModFolder()
    {
        using var fixture = await ModRuntimeFixture.CreateAsync();
        fixture.ReplaceOrdinaryFile("map/data.bin", "modded");
        fixture.AddFile("Mods/EnemyRandomizer/config.ini", "enabled=1");

        var result = await fixture.Service.ValidateAsync(CancellationToken.None);

        Assert.True(result.IsReady, string.Join(Environment.NewLine, result.Errors));
    }

    [Theory]
    [InlineData("DarkSoulsRemastered.exe")]
    [InlineData("steam_api64.dll")]
    public async Task ValidateAsync_RejectsProtectedCoreChange(string relativePath)
    {
        using var fixture = await ModRuntimeFixture.CreateAsync();
        fixture.ReplaceOrdinaryFile(relativePath, "changed");

        var result = await fixture.Service.ValidateAsync(CancellationToken.None);

        Assert.False(result.IsReady);
        Assert.Contains(result.Errors, error => error.Contains("hash", StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public async Task ValidateAsync_RejectsReparsePointAnywhereInRuntime()
    {
        using var fixture = await ModRuntimeFixture.CreateAsync();
        var link = Path.Combine(fixture.RuntimeRoot, "Mods", "linked");
        CreateReparseDirectory(link, fixture.OutsideRoot);
        try
        {
            var result = await fixture.Service.ValidateAsync(CancellationToken.None);

            Assert.False(result.IsReady);
            Assert.Contains(result.Errors, error => error.Contains("reparse", StringComparison.OrdinalIgnoreCase));
        }
        finally
        {
            DeleteReparseDirectory(link);
        }
    }

    [Fact]
    public async Task ValidateAsync_RejectsPointerOutsideSelectedRoot()
    {
        using var fixture = await ModRuntimeFixture.CreateAsync();
        await fixture.WritePointerAsync(new RuntimePointer(
            "runtime-escape",
            "../outside/runtime-escape",
            "hash"));

        var result = await fixture.Service.ValidateAsync(CancellationToken.None);

        Assert.False(result.IsReady);
        Assert.Contains(result.Errors, error => error.Contains("root", StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public async Task ValidateAsync_RejectsMissingManifest()
    {
        using var fixture = await ModRuntimeFixture.CreateAsync();
        File.Delete(fixture.ManifestPath);

        var result = await fixture.Service.ValidateAsync(CancellationToken.None);

        Assert.False(result.IsReady);
        Assert.Contains(result.Errors, error => error.Contains("manifest", StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public async Task ValidateAsync_RejectsChangedManifest()
    {
        using var fixture = await ModRuntimeFixture.CreateAsync();
        await File.AppendAllTextAsync(fixture.ManifestPath, " ");

        var result = await fixture.Service.ValidateAsync(CancellationToken.None);

        Assert.False(result.IsReady);
        Assert.Contains(result.Errors, error => error.Contains("manifest hash", StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public async Task ValidateAsync_RejectsCaseCollidingProtectedManifestNames()
    {
        using var fixture = await ModRuntimeFixture.CreateAsync();
        var manifest = await fixture.ReadManifestAsync();
        var duplicate = manifest.Files.Single(file => file.RelativePath == "DarkSoulsRemastered.exe") with
        {
            RelativePath = "darksoulsremastered.exe"
        };
        await fixture.WriteManifestAsync(manifest with { Files = manifest.Files.Append(duplicate).ToArray() });

        var result = await fixture.Service.ValidateAsync(CancellationToken.None);

        Assert.False(result.IsReady);
        Assert.Contains(result.Errors, error => error.Contains("duplicate", StringComparison.OrdinalIgnoreCase));
    }

    private sealed class ModRuntimeFixture : IDisposable
    {
        private readonly string _container;
        private readonly FileHashService _hashes = new();
        private readonly RuntimePointerStore _pointerStore;

        private ModRuntimeFixture(string container)
        {
            _container = container;
            OutsideRoot = Path.Combine(container, "outside");
            var sourceRoot = Path.Combine(container, "source");
            var localRoot = Path.Combine(container, "local");
            Directory.CreateDirectory(OutsideRoot);
            Directory.CreateDirectory(sourceRoot);
            Directory.CreateDirectory(localRoot);
            var canonicalizer = new WindowsPathCanonicalizer();
            var boundary = WriteBoundary.Create(sourceRoot, localRoot, canonicalizer);
            Layout = LocalDataLayout.Create(localRoot, boundary);
            RuntimeRoot = Path.Combine(Layout.Runtimes, "runtime-abc");
            ManifestPath = Path.Combine(RuntimeRoot, "runtime-manifest.json");
            _pointerStore = new RuntimePointerStore(Layout, boundary);
            Service = new ModRuntimeReadinessService(
                Layout,
                boundary,
                canonicalizer,
                _hashes,
                _pointerStore);
        }

        public string OutsideRoot { get; }

        public LocalDataLayout Layout { get; }

        public string RuntimeRoot { get; }

        public string ManifestPath { get; }

        public ModRuntimeReadinessService Service { get; }

        public static async Task<ModRuntimeFixture> CreateAsync()
        {
            var container = Path.Combine(Path.GetTempPath(), $"dsr-mod-readiness-{Guid.NewGuid():N}");
            Directory.CreateDirectory(container);
            var fixture = new ModRuntimeFixture(container);
            await fixture.CreateValidRuntimeAsync();
            return fixture;
        }

        public void ReplaceOrdinaryFile(string relativePath, string contents) =>
            File.WriteAllText(Path.Combine(RuntimeRoot, relativePath.Replace('/', Path.DirectorySeparatorChar)), contents);

        public void AddFile(string relativePath, string contents)
        {
            var path = Path.Combine(RuntimeRoot, relativePath.Replace('/', Path.DirectorySeparatorChar));
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, contents);
        }

        public async Task<RuntimeManifest> ReadManifestAsync()
        {
            await using var stream = File.OpenRead(ManifestPath);
            return (await JsonSerializer.DeserializeAsync<RuntimeManifest>(stream, JsonOptions))!;
        }

        public async Task WriteManifestAsync(RuntimeManifest manifest)
        {
            await File.WriteAllTextAsync(ManifestPath, JsonSerializer.Serialize(manifest, JsonOptions));
            await _pointerStore.ActivateAsync(new RuntimePointer(
                "runtime-abc",
                "runtimes/runtime-abc",
                await _hashes.ComputeSha256Async(ManifestPath, CancellationToken.None)),
                CancellationToken.None);
        }

        public Task WritePointerAsync(RuntimePointer pointer) =>
            File.WriteAllTextAsync(
                Path.Combine(Layout.Root, "runtime-current.json"),
                JsonSerializer.Serialize(pointer, JsonOptions));

        public void Dispose() => Directory.Delete(_container, recursive: true);

        private static JsonSerializerOptions JsonOptions { get; } = new()
        {
            PropertyNamingPolicy = JsonNamingPolicy.CamelCase
        };

        private async Task CreateValidRuntimeAsync()
        {
            AddFile("DarkSoulsRemastered.exe", "game");
            AddFile("steam_api64.dll", "steam");
            AddFile("map/data.bin", "stock");
            Directory.CreateDirectory(Path.Combine(RuntimeRoot, "Mods"));
            var paths = new[] { "DarkSoulsRemastered.exe", "steam_api64.dll", "map/data.bin" };
            var files = new List<RuntimeFileManifestEntry>();
            foreach (var relativePath in paths)
            {
                var path = Path.Combine(RuntimeRoot, relativePath.Replace('/', Path.DirectorySeparatorChar));
                files.Add(new RuntimeFileManifestEntry(
                    relativePath,
                    new FileInfo(path).Length,
                    await _hashes.ComputeSha256Async(path, CancellationToken.None)));
            }

            var manifest = new RuntimeManifest(
                1,
                "runtime-abc",
                new DateTimeOffset(2026, 8, 28, 0, 0, 0, TimeSpan.Zero),
                files[0].Sha256,
                "catalog-hash",
                files.Sum(file => file.Length),
                files);
            await WriteManifestAsync(manifest);
        }
    }

    private static void CreateReparseDirectory(string path, string target)
    {
        RunCmd($"mklink /J \"{path}\" \"{target}\"");
    }

    private static void DeleteReparseDirectory(string path) => RunCmd($"rmdir \"{path}\"");

    private static void RunCmd(string command)
    {
        using var process = Process.Start(new ProcessStartInfo("cmd.exe", $"/d /c {command}")
        {
            CreateNoWindow = true,
            RedirectStandardError = true,
            UseShellExecute = false
        }) ?? throw new IOException($"Unable to start cmd.exe for: {command}");
        var error = process.StandardError.ReadToEnd();
        process.WaitForExit();
        if (process.ExitCode != 0)
        {
            throw new IOException($"Command failed ({process.ExitCode}): {error}");
        }
    }
}
