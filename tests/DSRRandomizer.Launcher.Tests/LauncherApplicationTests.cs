using DSRRandomizer.Foundation.Installation;
using DSRRandomizer.Foundation.Runtime;
using DSRRandomizer.Launcher.Services;

namespace DSRRandomizer.Launcher.Tests;

public sealed class LauncherApplicationTests
{
    [Fact]
    public async Task RunAsync_LaunchArgumentIsRejected()
    {
        var output = new StringWriter();
        var application = new LauncherApplication(
            new FakeLauncherService(),
            output,
            new StringWriter());

        var exitCode = await application.RunAsync(new[] { "--launch" }, CancellationToken.None);

        Assert.Equal(2, exitCode);
        Assert.Contains("unsupported", output.ToString(), StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public async Task RunAsync_StatusReturnsReadinessFailureCode()
    {
        var application = new LauncherApplication(
            new FakeLauncherService
            {
                ReadinessResult = new RuntimeReadinessResult(false, null, new[] { "not ready" })
            },
            new StringWriter(),
            new StringWriter());

        var exitCode = await application.RunAsync(new[] { "--status" }, CancellationToken.None);

        Assert.Equal(5, exitCode);
    }

    [Fact]
    public void LauncherAndFoundationAssemblies_DoNotReferenceProcessStartingAssembly()
    {
        var assemblies = new[]
        {
            typeof(LauncherApplication).Assembly,
            typeof(RuntimeReadinessService).Assembly
        };

        Assert.All(assemblies, assembly => Assert.DoesNotContain(
            assembly.GetReferencedAssemblies(),
            reference => reference.Name == "System.Diagnostics.Process"));
    }

    [Fact]
    public void ProgramMain_LaunchRejectionReturnsWithoutEnteringWpfLifetime()
    {
        var exitCode = Program.Main(new[] { "--launch" });

        Assert.Equal(2, exitCode);
    }

    private sealed class FakeLauncherService : ILauncherService
    {
        public RuntimeReadinessResult ReadinessResult { get; init; } =
            new(true, @"C:\Local\runtime", Array.Empty<string>());

        public Task<VerificationResult> VerifyAsync(
            string gamePath,
            CancellationToken cancellationToken) =>
            Task.FromResult(new VerificationResult(
                true,
                gamePath,
                new GameFileCatalog(Array.Empty<GameFileEntry>(), 0),
                Array.Empty<string>()));

        public Task<RuntimeManifest> InitializeRuntimeAsync(
            string gamePath,
            IProgress<RuntimeBuildProgress>? progress,
            CancellationToken cancellationToken) =>
            throw new NotSupportedException();

        public Task<RuntimeReadinessResult> GetReadinessAsync(
            CancellationToken cancellationToken) =>
            Task.FromResult(ReadinessResult);
    }
}
