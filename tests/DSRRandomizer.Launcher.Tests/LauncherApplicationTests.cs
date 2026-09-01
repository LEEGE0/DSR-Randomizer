using DSRRandomizer.Foundation.Installation;
using DSRRandomizer.Foundation.Runtime;
using DSRRandomizer.Foundation.Saves;
using DSRRandomizer.Launcher.Services;
using DSRRandomizer.Launcher.Safety;
using System.Text.Json;

namespace DSRRandomizer.Launcher.Tests;

public sealed class LauncherApplicationTests
{
    [Fact]
    public async Task RunAsync_MaterialOperationWithoutExternalRootReturnsStableSelectionError()
    {
        var output = new StringWriter();
        var application = new LauncherApplication(
            service: null,
            output,
            new StringWriter(),
            externalRootSelected: false);

        var exitCode = await application.RunAsync(new[] { "--status" }, CancellationToken.None);

        Assert.Equal(8, exitCode);
        using var document = JsonDocument.Parse(output.ToString());
        Assert.Equal(
            "EXTERNAL_ROOT_NOT_SELECTED",
            document.RootElement.GetProperty("errorCode").GetString());
    }

    [Fact]
    public async Task RunAsync_LaunchWithoutExternalRootReturnsStableSelectionError()
    {
        var output = new StringWriter();
        var application = new LauncherApplication(
            service: null,
            output,
            new StringWriter(),
            externalRootSelected: false);

        var exitCode = await application.RunAsync(new[] { "--launch" }, CancellationToken.None);

        Assert.Equal(8, exitCode);
        using var document = JsonDocument.Parse(output.ToString());
        Assert.Equal(
            "EXTERNAL_ROOT_NOT_SELECTED",
            document.RootElement.GetProperty("errorCode").GetString());
    }

    [Theory]
    [InlineData("{malformed")]
    [InlineData("{\"schemaVersion\":1,\"root\":\"C:\\\\missing-external-root\"}")]
    public async Task RunWithLocalDataRoot_SetRootRepairsBrokenPreviousPointer(string previousPointer)
    {
        using var fixture = ProgramFixture.Create();
        await File.WriteAllTextAsync(Path.Combine(fixture.LocalRoot, "external-root.json"), previousPointer);
        var output = new StringWriter();

        var exitCode = Program.RunWithLocalDataRoot(
            new[] { "--set-root", fixture.ValidExternalRoot },
            fixture.LocalRoot,
            output,
            new StringWriter());

        Assert.Equal(0, exitCode);
        Assert.Equal(
            fixture.ValidExternalRoot,
            await new DSRRandomizer.Launcher.Configuration.ExternalRootSelectionStore(fixture.LocalRoot)
                .ReadAsync(CancellationToken.None));
    }

    [Fact]
    public async Task RunAsync_LaunchWithSteamIdCallsSharedService()
    {
        var output = new StringWriter();
        var service = new FakeLauncherService
        {
            LaunchResult = new SafetyLaunchResult(true, string.Empty, 0)
        };
        var application = new LauncherApplication(
            service,
            output,
            new StringWriter());

        var exitCode = await application.RunAsync(
            new[] { "--launch", "12345678901234567" },
            CancellationToken.None);

        Assert.Equal(0, exitCode);
        Assert.Equal("12345678901234567", Assert.Single(service.LaunchCalls));
        using var document = JsonDocument.Parse(output.ToString());
        Assert.True(document.RootElement.GetProperty("success").GetBoolean());
    }

    [Fact]
    public async Task RunAsync_LaunchAcceptsNineDigitNumericSaveFolder()
    {
        var service = new FakeLauncherService
        {
            LaunchResult = new SafetyLaunchResult(true, string.Empty, 0)
        };
        var application = new LauncherApplication(
            service,
            new StringWriter(),
            new StringWriter());

        var exitCode = await application.RunAsync(
            new[] { "--launch", "123456789" },
            CancellationToken.None);

        Assert.Equal(0, exitCode);
        Assert.Equal("123456789", Assert.Single(service.LaunchCalls));
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
    public async Task RunAsync_PrepareSaveRejectsNonDecimalSteamIdWithoutCallingService()
    {
        var service = new FakeLauncherService();
        var application = new LauncherApplication(
            service,
            new StringWriter(),
            new StringWriter());

        var exitCode = await application.RunAsync(
            new[] { "--prepare-save", "1234-not-a-steam-id" },
            CancellationToken.None);

        Assert.Equal(2, exitCode);
        Assert.Empty(service.PrepareCalls);
    }

    [Fact]
    public async Task RunAsync_PrepareSaveFirstCopyRequiredReturnsStableNonzeroCode()
    {
        var output = new StringWriter();
        var service = new FakeLauncherService
        {
            PrepareResult = DedicatedSaveResult.Fail(
                SaveErrorCode.FirstCopyConfirmationRequired,
                "First-copy confirmation is required in the UI.")
        };
        var application = new LauncherApplication(service, output, new StringWriter());

        var exitCode = await application.RunAsync(
            new[] { "--prepare-save", "12345678901234567" },
            CancellationToken.None);

        Assert.Equal(7, exitCode);
        Assert.Equal(("12345678901234567", false), Assert.Single(service.PrepareCalls));
        using var document = JsonDocument.Parse(output.ToString());
        Assert.Equal(
            10,
            document.RootElement.GetProperty("errorCode").GetInt32());
        Assert.Contains(
            "confirmation",
            document.RootElement.GetProperty("error").GetString(),
            StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public async Task RunAsync_PrepareSaveExistingRmmReturnsSuccessWithoutLaunching()
    {
        var output = new StringWriter();
        var destination = @"C:\Local\saves\12345678901234567\DRAKS0005.rmm";
        var service = new FakeLauncherService
        {
            PrepareResult = new DedicatedSaveResult(
                true,
                true,
                destination,
                SaveErrorCode.None,
                string.Empty)
        };
        var application = new LauncherApplication(service, output, new StringWriter());

        var exitCode = await application.RunAsync(
            new[] { "--prepare-save", "12345678901234567" },
            CancellationToken.None);

        Assert.Equal(0, exitCode);
        Assert.Equal(("12345678901234567", false), Assert.Single(service.PrepareCalls));
        using var document = JsonDocument.Parse(output.ToString());
        Assert.Equal(destination, document.RootElement.GetProperty("savePath").GetString());
        Assert.True(document.RootElement.GetProperty("reusedExisting").GetBoolean());
    }

    [Fact]
    public async Task RunAsync_PrepareSaveIoFailureReturnsStablePreparationCode()
    {
        var output = new StringWriter();
        var service = new FakeLauncherService
        {
            PrepareException = new IOException("local save state unavailable")
        };
        var application = new LauncherApplication(service, output, new StringWriter());

        var exitCode = await application.RunAsync(
            new[] { "--prepare-save", "12345678901234567" },
            CancellationToken.None);

        Assert.Equal(7, exitCode);
        Assert.Contains("local save state unavailable", output.ToString(), StringComparison.Ordinal);
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
    public void ProductLaunch_WithoutSelectedExternalRootReturnsSelectionError()
    {
        using var fixture = ProgramFixture.Create();
        var output = new StringWriter();

        var exitCode = Program.RunWithLocalDataRoot(
            new[] { "--launch" },
            fixture.LocalRoot,
            output,
            new StringWriter());

        Assert.Equal(8, exitCode);
        Assert.Contains("EXTERNAL_ROOT_NOT_SELECTED", output.ToString(), StringComparison.Ordinal);
    }

    private sealed class FakeLauncherService : ILauncherService
    {
        public DedicatedSaveResult PrepareResult { get; init; } =
            DedicatedSaveResult.Fail(SaveErrorCode.SourceMissing, "not configured");

        public Exception? PrepareException { get; init; }

        public List<(string SteamId, bool FirstCopyConfirmed)> PrepareCalls { get; } = [];

        public RuntimeReadinessResult ReadinessResult { get; init; } =
            new(true, @"C:\Local\runtime", Array.Empty<string>());

        public SafetyLaunchResult LaunchResult { get; init; } =
            SafetyLaunchResult.Failed("not configured");

        public List<string> LaunchCalls { get; } = [];

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

        public Task<RuntimeReadinessResult> GetModdedLaunchReadinessAsync(
            CancellationToken cancellationToken) =>
            Task.FromResult(ReadinessResult);

        public Task<IReadOnlyList<SaveProfileCandidate>> DiscoverSaveProfilesAsync(
            CancellationToken cancellationToken) =>
            Task.FromResult<IReadOnlyList<SaveProfileCandidate>>([]);

        public Task<DedicatedSaveResult> PrepareDedicatedSaveAsync(
            string steamId,
            bool firstCopyConfirmed,
            CancellationToken cancellationToken)
        {
            PrepareCalls.Add((steamId, firstCopyConfirmed));
            if (PrepareException is not null)
            {
                return Task.FromException<DedicatedSaveResult>(PrepareException);
            }

            return Task.FromResult(PrepareResult);
        }

        public Task<SafetyLaunchResult> LaunchModdedAsync(
            string steamId,
            CancellationToken cancellationToken)
        {
            LaunchCalls.Add(steamId);
            return Task.FromResult(LaunchResult);
        }

        public Task<RandomizerToolLaunchResult> LaunchItemRandomizerAsync(
            CancellationToken cancellationToken) =>
            throw new NotSupportedException();

        public Task<RandomizerToolLaunchResult> LaunchEnemyRandomizerAsync(
            CancellationToken cancellationToken) =>
            throw new NotSupportedException();
    }

    private sealed class ProgramFixture : IDisposable
    {
        private ProgramFixture(string container)
        {
            Container = container;
            LocalRoot = Path.Combine(container, "local");
            ValidExternalRoot = Path.Combine(container, "external");
            Directory.CreateDirectory(LocalRoot);
            Directory.CreateDirectory(ValidExternalRoot);
        }

        public string Container { get; }

        public string LocalRoot { get; }

        public string ValidExternalRoot { get; }

        public static ProgramFixture Create()
        {
            var container = Path.Combine(Path.GetTempPath(), $"dsr-program-{Guid.NewGuid():N}");
            Directory.CreateDirectory(container);
            return new ProgramFixture(container);
        }

        public void Dispose() => Directory.Delete(Container, recursive: true);
    }
}
