using DSRRandomizer.Foundation.Installation;
using DSRRandomizer.Foundation.Runtime;
using DSRRandomizer.Foundation.Saves;
using DSRRandomizer.Launcher.Logging;
using DSRRandomizer.Launcher.Configuration;
using DSRRandomizer.Launcher.Services;
using DSRRandomizer.Launcher.ViewModels;
using DSRRandomizer.Launcher.Safety;

namespace DSRRandomizer.Launcher.Tests.ViewModels;

public sealed class MainWindowViewModelTests
{
    private const string SteamId = "12345678901234567";

    [Fact]
    public async Task SaveExternalRootCommand_PersistsSelectionAndRequiresRestart()
    {
        using var fixture = ExternalRootFixture.Create();
        var viewModel = new MainWindowViewModel(
            service: null,
            new RecordingLogger(),
            materialRoot: string.Empty,
            fixture.Store,
            materialOperationsAvailable: false)
        {
            ExternalRootPath = fixture.ExternalRoot
        };

        await viewModel.SaveExternalRootCommand.ExecuteAsync(null);

        Assert.Equal(fixture.ExternalRoot, await fixture.Store.ReadAsync(CancellationToken.None));
        Assert.Contains("restart", viewModel.Status, StringComparison.OrdinalIgnoreCase);
        Assert.False(viewModel.CanInitialize);
    }

    [Fact]
    public async Task SaveExternalRootCommand_ChangedRootDisablesAllMaterialCommandsUntilRestart()
    {
        using var fixture = ExternalRootFixture.Create();
        var newRoot = Path.Combine(fixture.Container, "different-external");
        Directory.CreateDirectory(newRoot);
        var viewModel = new MainWindowViewModel(
            new FakeLauncherService(),
            new RecordingLogger(),
            fixture.ExternalRoot,
            fixture.Store)
        {
            GamePath = @"C:\NotTheSource",
            ExternalRootPath = newRoot
        };

        Assert.True(viewModel.VerifyCommand.CanExecute(null));
        Assert.True(viewModel.InitializeCommand.CanExecute(null));
        Assert.True(viewModel.AreSaveControlsEnabled);
        await viewModel.SaveExternalRootCommand.ExecuteAsync(null);

        Assert.False(viewModel.VerifyCommand.CanExecute(null));
        Assert.False(viewModel.InitializeCommand.CanExecute(null));
        Assert.False(viewModel.AreSaveControlsEnabled);
        Assert.Contains("restart", viewModel.Status, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public async Task InitializeCommand_MarksModRuntimeReadyButStillRequiresSelectedProfile()
    {
        var service = new FakeLauncherService();
        var viewModel = CreateViewModel(service, new RecordingLogger());
        viewModel.GamePath = @"C:\Steam\DSR";

        await viewModel.InitializeCommand.ExecuteAsync(null);

        Assert.False(viewModel.CanLaunch);
        Assert.Equal(
            "External mod runtime is ready. Select a SteamID to launch after placing Steam in Offline Mode.",
            viewModel.Status);
        Assert.False(viewModel.IsBusy);
        Assert.Equal(100, viewModel.ProgressPercent);
    }

    [Fact]
    public async Task InitializeCommand_SingleProfileEnablesAutomaticBootstrapLaunchWithoutPreparing()
    {
        var service = new FakeLauncherService
        {
            SaveProfiles = [new SaveProfileCandidate(
                SteamId,
                @"C:\Documents\12345678901234567\DRAKS0005.sl2")]
        };
        var viewModel = CreateViewModel(service, new RecordingLogger());
        viewModel.GamePath = @"C:\Steam\DSR";

        await viewModel.InitializeCommand.ExecuteAsync(null);

        Assert.True(viewModel.CanLaunch);
        Assert.Contains("automatically", viewModel.Status, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public async Task InitializeCommand_FailureLeavesLaunchDisabledAndLogsException()
    {
        var logger = new RecordingLogger();
        var service = new FakeLauncherService
        {
            RuntimeException = new IOException("copy failed")
        };
        var viewModel = CreateViewModel(service, logger);
        viewModel.GamePath = @"C:\Steam\DSR";

        await viewModel.InitializeCommand.ExecuteAsync(null);

        Assert.False(viewModel.CanLaunch);
        Assert.Equal("Runtime creation failed: copy failed", viewModel.Status);
        Assert.False(viewModel.IsBusy);
        Assert.Single(logger.Exceptions);
        Assert.IsType<IOException>(logger.Exceptions[0]);
    }

    [Fact]
    public async Task VerifyCommand_ReportsAllVerificationErrorsWithoutEnablingLaunch()
    {
        var service = new FakeLauncherService
        {
            VerificationResult = new VerificationResult(
                false,
                string.Empty,
                null,
                new[] { "missing executable", "missing sound" })
        };
        var viewModel = CreateViewModel(service, new RecordingLogger());
        viewModel.GamePath = @"C:\Broken";

        await viewModel.VerifyCommand.ExecuteAsync(null);

        Assert.Equal("Installation verification failed: missing executable; missing sound", viewModel.Status);
        Assert.False(viewModel.CanLaunch);
    }

    [Fact]
    public async Task LoadAsync_SingleNormalProfileEnablesLaunchWithoutPreparing()
    {
        var source = @"C:\Documents\12345678901234567\DRAKS0005.sl2";
        var service = new FakeLauncherService
        {
            SaveProfiles = [new SaveProfileCandidate(SteamId, source)]
        };
        var viewModel = CreateViewModel(service, new RecordingLogger());

        await viewModel.LoadAsync();

        Assert.True(viewModel.CanLaunch);
        Assert.Equal(SteamId, viewModel.SelectedSaveProfile?.SteamId);
        Assert.Equal(source, viewModel.SelectedSaveSourcePath);
        Assert.Contains("automatically", viewModel.Status, StringComparison.OrdinalIgnoreCase);
        Assert.Contains(".sl2", viewModel.Status, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public async Task LoadAsync_MultipleProfilesEnablesLaunchImmediatelyAfterExplicitSelection()
    {
        var first = new SaveProfileCandidate(
            "12345678901234567",
            @"C:\Documents\12345678901234567\DRAKS0005.sl2");
        var second = new SaveProfileCandidate(
            "76543210987654321",
            @"C:\Documents\76543210987654321\DRAKS0005.sl2");
        var service = new FakeLauncherService { SaveProfiles = [first, second] };
        var viewModel = CreateViewModel(service, new RecordingLogger());

        await viewModel.LoadAsync();

        Assert.Equal(2, viewModel.SaveProfiles.Count);
        Assert.Null(viewModel.SelectedSaveProfile);
        Assert.False(viewModel.CanLaunch);
        viewModel.SelectedSaveProfile = second;

        Assert.True(viewModel.CanLaunch);
    }

    [Fact]
    public async Task LaunchCommand_ReadyRuntimeAndSelectedProfileCallsSharedLaunchService()
    {
        var profile = new SaveProfileCandidate(
            SteamId,
            @"C:\Documents\12345678901234567\DRAKS0005.sl2");
        var service = new FakeLauncherService
        {
            SaveProfiles = [profile],
            LaunchResult = new SafetyLaunchResult(true, string.Empty, 0)
        };
        var viewModel = CreateViewModel(service, new RecordingLogger());
        await viewModel.LoadAsync();

        Assert.True(viewModel.LaunchCommand.CanExecute(null));
        await viewModel.LaunchCommand.ExecuteAsync(null);

        Assert.Equal(SteamId, Assert.Single(service.LaunchCalls));
        Assert.Contains("exited", viewModel.Status, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public async Task LoadAsync_ModReadyRuntimeAndValidExistingRmmEnablesLaunchWithoutPrepareCommand()
    {
        var service = new FakeLauncherService
        {
            SaveProfiles = [new SaveProfileCandidate(SteamId, string.Empty)]
        };
        var viewModel = CreateViewModel(service, new RecordingLogger());

        await viewModel.LoadAsync();

        Assert.True(viewModel.LaunchCommand.CanExecute(null));
    }

    private static MainWindowViewModel CreateViewModel(
        ILauncherService service,
        IExternalLogger logger) =>
        new(service, logger, Path.Combine(Path.GetTempPath(), "DSR-Randomizer-Launcher-Tests"));

    private sealed class FakeLauncherService : ILauncherService
    {
        public IReadOnlyList<SaveProfileCandidate> SaveProfiles { get; init; } = [];

        public VerificationResult VerificationResult { get; init; } = new(
            true,
            @"C:\Steam\DSR",
            new GameFileCatalog(Array.Empty<GameFileEntry>(), 0),
            Array.Empty<string>());

        public Exception? RuntimeException { get; init; }

        public SafetyLaunchResult LaunchResult { get; init; } =
            SafetyLaunchResult.Failed("not configured");

        public List<string> LaunchCalls { get; } = [];

        public Task<VerificationResult> VerifyAsync(
            string gamePath,
            CancellationToken cancellationToken) =>
            Task.FromResult(VerificationResult);

        public Task<RuntimeManifest> InitializeRuntimeAsync(
            string gamePath,
            IProgress<RuntimeBuildProgress>? progress,
            CancellationToken cancellationToken)
        {
            if (RuntimeException is not null)
            {
                return Task.FromException<RuntimeManifest>(RuntimeException);
            }

            progress?.Report(new RuntimeBuildProgress(1, 1, "DarkSoulsRemastered.exe"));
            return Task.FromResult(new RuntimeManifest(
                1,
                "runtime-test",
                DateTimeOffset.UtcNow,
                "exe-hash",
                "catalog-hash",
                1,
                Array.Empty<RuntimeFileManifestEntry>())
            {
                RuntimePath = @"C:\Local\runtime-test"
            });
        }

        public Task<RuntimeReadinessResult> GetReadinessAsync(
            CancellationToken cancellationToken) =>
            Task.FromResult(new RuntimeReadinessResult(
                true,
                @"C:\Local\runtime-test",
                Array.Empty<string>()));

        public Task<RuntimeReadinessResult> GetModdedLaunchReadinessAsync(
            CancellationToken cancellationToken) => GetReadinessAsync(cancellationToken);

        public Task<IReadOnlyList<SaveProfileCandidate>> DiscoverSaveProfilesAsync(
            CancellationToken cancellationToken) =>
            Task.FromResult(SaveProfiles);

        public Task<DedicatedSaveResult> PrepareDedicatedSaveAsync(
            string steamId,
            bool firstCopyConfirmed,
            CancellationToken cancellationToken) =>
            throw new InvalidOperationException(
                "WPF must not explicitly prepare the dedicated save.");

        public Task<SafetyLaunchResult> LaunchModdedAsync(
            string steamId,
            CancellationToken cancellationToken)
        {
            LaunchCalls.Add(steamId);
            return Task.FromResult(LaunchResult);
        }
    }

    private sealed class RecordingLogger : IExternalLogger
    {
        public List<Exception> Exceptions { get; } = new();

        public Task LogExceptionAsync(Exception exception, CancellationToken cancellationToken)
        {
            Exceptions.Add(exception);
            return Task.CompletedTask;
        }
    }

    private sealed class ExternalRootFixture : IDisposable
    {
        private ExternalRootFixture(string container)
        {
            Container = container;
            LocalRoot = Path.Combine(container, "local");
            ExternalRoot = Path.Combine(container, "external");
            Directory.CreateDirectory(LocalRoot);
            Directory.CreateDirectory(ExternalRoot);
            Store = new ExternalRootSelectionStore(LocalRoot);
        }

        public string Container { get; }

        public string LocalRoot { get; }

        public string ExternalRoot { get; }

        public ExternalRootSelectionStore Store { get; }

        public static ExternalRootFixture Create()
        {
            var container = Path.Combine(Path.GetTempPath(), $"dsr-external-root-view-model-{Guid.NewGuid():N}");
            Directory.CreateDirectory(container);
            return new ExternalRootFixture(container);
        }

        public void Dispose() => Directory.Delete(Container, recursive: true);
    }
}
