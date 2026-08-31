using DSRRandomizer.Foundation.Installation;
using DSRRandomizer.Foundation.Runtime;
using DSRRandomizer.Foundation.Saves;
using DSRRandomizer.Launcher.Services;
using DSRRandomizer.Launcher.Safety;
using System.Security.Cryptography;

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
            Path.Combine(_packageRoot, "DSRForMod.Launcher.exe"),
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
    [InlineData("DSRForMod.Launcher.exe")]
    [InlineData("README.md")]
    [InlineData("LICENSE")]
    [InlineData("CHANGELOG.md")]
    [InlineData("THIRD_PARTY_NOTICES.md")]
    [InlineData("config/compatibility-profiles.json")]
    [InlineData("native/DSRRandomizer.Runtime.dll")]
    [InlineData("native/DSRRandomizer.Runtime.dll.sha256")]
    [InlineData("INSTALL_KO.md")]
    [InlineData("components/rmm-bridge/DSRRandomizer.RmmBridge.dll")]
    [InlineData("components/rmm-bridge/DSRRandomizer.RmmBridgeHost.exe")]
    [InlineData("components/rmm-bridge/deployment-manifest.json")]
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

    [Theory]
    [InlineData(" README.md")]
    [InlineData("README.md ")]
    public async Task RunAsync_ValidatePackageRejectsWhitespaceAliasAndReportsExactReadmeMissing(
        string alias)
    {
        await CreateCompletePackageAsync();
        var destination = Path.Combine(_packageRoot, alias);
        if (alias.EndsWith(' '))
        {
            destination = @"\\?\" + destination;
        }
        File.Move(Path.Combine(_packageRoot, "README.md"), destination);
        var output = new StringWriter();
        var application = new LauncherApplication(
            new UnusedLauncherService(),
            output,
            new StringWriter());

        var exitCode = await application.RunAsync(
            new[] { "--validate-package", _packageRoot },
            CancellationToken.None);

        Assert.Equal(6, exitCode);
        Assert.Contains(alias, output.ToString(), StringComparison.Ordinal);
        Assert.Contains("missing:README.md", output.ToString(), StringComparison.Ordinal);
    }

    [Fact]
    public async Task RunAsync_ValidatePackageRejectsGuardAndMatchingTamperedSidecar()
    {
        await CreateCompletePackageAsync();
        var guardPath = PackagePath("native/DSRRandomizer.Runtime.dll");
        var tampered = "tampered packaged guard"u8.ToArray();
        await File.WriteAllBytesAsync(guardPath, tampered);
        await File.WriteAllTextAsync(
            PackagePath("native/DSRRandomizer.Runtime.dll.sha256"),
            $"{Convert.ToHexString(SHA256.HashData(tampered)).ToLowerInvariant()}\n");

        var (exitCode, output) = await ValidatePackageAsync();

        Assert.Equal(6, exitCode);
        Assert.Contains(
            "mismatch:native/DSRRandomizer.Runtime.dll",
            output,
            StringComparison.Ordinal);
    }

    [Fact]
    public async Task RunAsync_ValidatePackageRejectsTamperedCompatibilityProfile()
    {
        await CreateCompletePackageAsync();
        await File.WriteAllTextAsync(
            PackagePath("config/compatibility-profiles.json"),
            "{\"tampered\":true}");

        var (exitCode, output) = await ValidatePackageAsync();

        Assert.Equal(6, exitCode);
        Assert.Contains(
            "mismatch:config/compatibility-profiles.json",
            output,
            StringComparison.Ordinal);
    }

    [Fact]
    public async Task RunAsync_ValidatePackageRejectsSidecarThatDoesNotNameStagedGuard()
    {
        await CreateCompletePackageAsync();
        await File.WriteAllTextAsync(
            PackagePath("native/DSRRandomizer.Runtime.dll.sha256"),
            $"{new string('0', 64)}\n");

        var (exitCode, output) = await ValidatePackageAsync();

        Assert.Equal(6, exitCode);
        Assert.Contains(
            "mismatch:native/DSRRandomizer.Runtime.dll.sha256",
            output,
            StringComparison.Ordinal);
    }

    [Fact]
    public async Task RunAsync_ValidatePackageRejectsMalformedGuardSidecar()
    {
        await CreateCompletePackageAsync();
        await File.WriteAllTextAsync(
            PackagePath("native/DSRRandomizer.Runtime.dll.sha256"),
            "not-a-sha256\n");

        var (exitCode, output) = await ValidatePackageAsync();

        Assert.Equal(6, exitCode);
        Assert.Contains(
            "mismatch:native/DSRRandomizer.Runtime.dll.sha256",
            output,
            StringComparison.Ordinal);
    }

    [Theory]
    [InlineData("components/rmm-bridge/DSRRandomizer.RmmBridge.dll", "mismatch:components/rmm-bridge/DSRRandomizer.RmmBridge.dll")]
    [InlineData("components/rmm-bridge/DSRRandomizer.RmmBridgeHost.exe", "mismatch:components/rmm-bridge/DSRRandomizer.RmmBridgeHost.exe")]
    public async Task RunAsync_ValidatePackageRejectsTamperedPinnedBridgeArtifacts(
        string artifactPath,
        string expectedFailure)
    {
        await CreateCompletePackageAsync();
        await File.WriteAllTextAsync(PackagePath(artifactPath), "tampered bridge artifact");

        var (exitCode, output) = await ValidatePackageAsync();

        Assert.Equal(6, exitCode);
        Assert.Contains(expectedFailure, output, StringComparison.Ordinal);
    }

    [Fact]
    public async Task RunAsync_ValidatePackageRejectsManifestThatDoesNotNamePinnedBridgeArtifacts()
    {
        await CreateCompletePackageAsync();
        await File.WriteAllTextAsync(
            PackagePath("components/rmm-bridge/deployment-manifest.json"),
            "{\"schemaVersion\":1,\"configuration\":\"Release\",\"runtimeId\":\"win-x64\",\"bridgeSha256\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"hostSha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}");

        var (exitCode, output) = await ValidatePackageAsync();

        Assert.Equal(6, exitCode);
        Assert.Contains(
            "mismatch:components/rmm-bridge/deployment-manifest.json",
            output,
            StringComparison.Ordinal);
    }

    private async Task CreateCompletePackageAsync()
    {
        Directory.CreateDirectory(_packageRoot);
        foreach (var path in new[]
                 {
                     "DSRForMod.Launcher.exe",
                     "README.md",
                     "INSTALL_KO.md",
                     "LICENSE",
                     "THIRD_PARTY_NOTICES.md",
                     "CHANGELOG.md"
                 })
        {
            var fullPath = PackagePath(path);
            Directory.CreateDirectory(Path.GetDirectoryName(fullPath)!);
            await File.WriteAllTextAsync(fullPath, path);
        }

        var guardPath = PackagePath("native/DSRRandomizer.Runtime.dll");
        var profilePath = PackagePath("config/compatibility-profiles.json");
        Directory.CreateDirectory(Path.GetDirectoryName(guardPath)!);
        Directory.CreateDirectory(Path.GetDirectoryName(profilePath)!);
        File.Copy(
            Path.Combine(AppContext.BaseDirectory, "native", "DSRRandomizer.Runtime.dll"),
            guardPath);
        File.Copy(
            Path.Combine(AppContext.BaseDirectory, "config", "compatibility-profiles.json"),
            profilePath);
        var bridgePath = PackagePath("components/rmm-bridge/DSRRandomizer.RmmBridge.dll");
        var hostPath = PackagePath("components/rmm-bridge/DSRRandomizer.RmmBridgeHost.exe");
        Directory.CreateDirectory(Path.GetDirectoryName(bridgePath)!);
        File.Copy(
            Path.Combine(AppContext.BaseDirectory, "components", "rmm-bridge", "DSRRandomizer.RmmBridge.dll"),
            bridgePath);
        File.Copy(
            Path.Combine(AppContext.BaseDirectory, "components", "rmm-bridge", "DSRRandomizer.RmmBridgeHost.exe"),
            hostPath);
        var guardHash = Convert.ToHexString(SHA256.HashData(await File.ReadAllBytesAsync(guardPath)))
            .ToLowerInvariant();
        await File.WriteAllTextAsync(
            PackagePath("native/DSRRandomizer.Runtime.dll.sha256"),
            $"{guardHash}\n");
        var bridgeHash = Convert.ToHexString(SHA256.HashData(await File.ReadAllBytesAsync(bridgePath)))
            .ToLowerInvariant();
        var hostHash = Convert.ToHexString(SHA256.HashData(await File.ReadAllBytesAsync(hostPath)))
            .ToLowerInvariant();
        await File.WriteAllTextAsync(
            PackagePath("components/rmm-bridge/deployment-manifest.json"),
            $"{{\"schemaVersion\":1,\"configuration\":\"Release\",\"runtimeId\":\"win-x64\",\"bridgeSha256\":\"{bridgeHash}\",\"hostSha256\":\"{hostHash}\"}}");
    }

    private async Task<(int ExitCode, string Output)> ValidatePackageAsync()
    {
        var output = new StringWriter();
        var application = new LauncherApplication(
            new UnusedLauncherService(),
            output,
            new StringWriter());
        var exitCode = await application.RunAsync(
            new[] { "--validate-package", _packageRoot },
            CancellationToken.None);
        return (exitCode, output.ToString());
    }

    private string PackagePath(string relativePath) => Path.Combine(
        _packageRoot,
        relativePath.Replace('/', Path.DirectorySeparatorChar));

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

        public Task<RandomizerToolLaunchResult> LaunchItemRandomizerAsync(
            CancellationToken cancellationToken) =>
            throw new InvalidOperationException("Package validation must not launch a process.");

        public Task<RandomizerToolLaunchResult> LaunchEnemyRandomizerAsync(
            CancellationToken cancellationToken) =>
            throw new InvalidOperationException("Package validation must not launch a process.");
    }
}
