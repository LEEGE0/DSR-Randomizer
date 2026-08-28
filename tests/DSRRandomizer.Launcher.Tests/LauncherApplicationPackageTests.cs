using DSRRandomizer.Foundation.Installation;
using DSRRandomizer.Foundation.Runtime;
using DSRRandomizer.Foundation.Saves;
using DSRRandomizer.Launcher.Services;
using DSRRandomizer.Launcher.Safety;

namespace DSRRandomizer.Launcher.Tests;

public sealed class LauncherApplicationPackageTests : IDisposable
{
    private readonly string _packageRoot = Path.Combine(
        Path.GetTempPath(),
        $"dsr-package-{Guid.NewGuid():N}");

    [Fact]
    public async Task RunAsync_ValidatePackageNamesProhibitedGameExecutableAndReturnsSix()
    {
        Directory.CreateDirectory(_packageRoot);
        await File.WriteAllTextAsync(
            Path.Combine(_packageRoot, "DSRRandomizer.Launcher.exe"),
            "launcher");
        await File.WriteAllTextAsync(
            Path.Combine(_packageRoot, "DarkSoulsRemastered.exe"),
            "game");
        var output = new StringWriter();
        var application = new LauncherApplication(
            new UnusedLauncherService(),
            output,
            new StringWriter());

        var exitCode = await application.RunAsync(
            new[] { "--validate-package", _packageRoot },
            CancellationToken.None);

        Assert.Equal(6, exitCode);
        Assert.Contains("DarkSoulsRemastered.exe", output.ToString(), StringComparison.Ordinal);
    }

    [Fact]
    public async Task RunAsync_ValidatePackageAcceptsExactAllowlist()
    {
        await CreateCompletePackageAsync();

        var application = new LauncherApplication(
            new UnusedLauncherService(),
            new StringWriter(),
            new StringWriter());

        Assert.Equal(0, await application.RunAsync(
            new[] { "--validate-package", _packageRoot },
            CancellationToken.None));
    }

    [Theory]
    [InlineData("DSRRandomizer.Launcher.exe")]
    [InlineData("README.md")]
    [InlineData("LICENSE")]
    [InlineData("CHANGELOG.md")]
    [InlineData("THIRD_PARTY_NOTICES.md")]
    [InlineData("config/compatibility-profiles.json")]
    [InlineData("native/DSRRandomizer.Runtime.dll")]
    [InlineData("native/DSRRandomizer.Runtime.dll.sha256")]
    public async Task RunAsync_ValidatePackageRejectsEachMissingRequiredArtifact(
        string missingPath)
    {
        await CreateCompletePackageAsync();
        File.Delete(Path.Combine(
            _packageRoot,
            missingPath.Replace('/', Path.DirectorySeparatorChar)));
        var output = new StringWriter();
        var application = new LauncherApplication(
            new UnusedLauncherService(),
            output,
            new StringWriter());

        var exitCode = await application.RunAsync(
            new[] { "--validate-package", _packageRoot },
            CancellationToken.None);

        Assert.Equal(6, exitCode);
        Assert.Contains($"missing:{missingPath}", output.ToString(), StringComparison.Ordinal);
    }

    private async Task CreateCompletePackageAsync()
    {
        Directory.CreateDirectory(_packageRoot);
        foreach (var path in new[]
                 {
                     "DSRRandomizer.Launcher.exe",
                     "README.md",
                     "LICENSE",
                     "THIRD_PARTY_NOTICES.md",
                     "CHANGELOG.md",
                     "native/DSRRandomizer.Runtime.dll",
                     "native/DSRRandomizer.Runtime.dll.sha256",
                     "config/compatibility-profiles.json"
                 })
        {
            var fullPath = Path.Combine(
                _packageRoot,
                path.Replace('/', Path.DirectorySeparatorChar));
            Directory.CreateDirectory(Path.GetDirectoryName(fullPath)!);
            await File.WriteAllTextAsync(fullPath, path);
        }
    }

    public void Dispose()
    {
        if (Directory.Exists(_packageRoot))
        {
            Directory.Delete(_packageRoot, recursive: true);
        }
    }

    private sealed class UnusedLauncherService : ILauncherService
    {
        public Task<VerificationResult> VerifyAsync(string gamePath, CancellationToken cancellationToken) =>
            throw new InvalidOperationException("Package validation must not call the game verifier.");

        public Task<RuntimeManifest> InitializeRuntimeAsync(
            string gamePath,
            IProgress<RuntimeBuildProgress>? progress,
            CancellationToken cancellationToken) =>
            throw new InvalidOperationException("Package validation must not build a runtime.");

        public Task<RuntimeReadinessResult> GetReadinessAsync(CancellationToken cancellationToken) =>
            throw new InvalidOperationException("Package validation must not read runtime status.");

        public Task<RuntimeReadinessResult> GetModdedLaunchReadinessAsync(
            CancellationToken cancellationToken) =>
            throw new InvalidOperationException("Package validation must not read mod runtime status.");

        public Task<IReadOnlyList<SaveProfileCandidate>> DiscoverSaveProfilesAsync(
            CancellationToken cancellationToken) =>
            throw new InvalidOperationException("Package validation must not discover save profiles.");

        public Task<DedicatedSaveResult> PrepareDedicatedSaveAsync(
            string steamId,
            bool firstCopyConfirmed,
            CancellationToken cancellationToken) =>
            throw new InvalidOperationException("Package validation must not prepare a save.");

        public Task<SafetyLaunchResult> LaunchModdedAsync(
            string steamId,
            CancellationToken cancellationToken) =>
            throw new InvalidOperationException("Package validation must not launch a process.");
    }
}
