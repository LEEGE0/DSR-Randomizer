using DSRRandomizer.Foundation.Installation;
using DSRRandomizer.Foundation.Runtime;
using DSRRandomizer.Launcher.Services;

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
        Directory.CreateDirectory(_packageRoot);
        foreach (var file in new[]
                 {
                     "DSRRandomizer.Launcher.exe",
                     "README.md",
                     "LICENSE",
                     "THIRD_PARTY_NOTICES.md",
                     "CHANGELOG.md"
                 })
        {
            await File.WriteAllTextAsync(Path.Combine(_packageRoot, file), file);
        }

        var application = new LauncherApplication(
            new UnusedLauncherService(),
            new StringWriter(),
            new StringWriter());

        Assert.Equal(0, await application.RunAsync(
            new[] { "--validate-package", _packageRoot },
            CancellationToken.None));
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
    }
}
