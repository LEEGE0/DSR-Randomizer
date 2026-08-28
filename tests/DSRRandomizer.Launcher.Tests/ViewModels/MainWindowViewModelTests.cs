using DSRRandomizer.Foundation.Installation;
using DSRRandomizer.Foundation.Runtime;
using DSRRandomizer.Foundation.Saves;
using DSRRandomizer.Launcher.Logging;
using DSRRandomizer.Launcher.Configuration;
using DSRRandomizer.Launcher.Services;
using DSRRandomizer.Launcher.ViewModels;

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
        Assert.True(viewModel.PrepareSaveCommand.CanExecute(null));
        await viewModel.SaveExternalRootCommand.ExecuteAsync(null);

        Assert.False(viewModel.VerifyCommand.CanExecute(null));
        Assert.False(viewModel.InitializeCommand.CanExecute(null));
        Assert.False(viewModel.PrepareSaveCommand.CanExecute(null));
        Assert.Contains("restart", viewModel.Status, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public async Task InitializeCommand_NativeFoundationStillKeepsPublicLaunchLocked()
    {
        var service = new FakeLauncherService();
        var viewModel = CreateViewModel(service, new RecordingLogger());
        viewModel.GamePath = @"C:\Steam\DSR";

        await viewModel.InitializeCommand.ExecuteAsync(null);

        Assert.False(viewModel.CanLaunch);
        Assert.Equal(
            "External runtime is ready. Launch stays locked until dedicated-save and online-blocking safety is installed.",
            viewModel.Status);
        Assert.False(viewModel.IsBusy);
        Assert.Equal(100, viewModel.ProgressPercent);
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
    public async Task PrepareSaveCommand_ZeroProfilesReportsMissingProfileWithoutPreparing()
    {
        var service = new FakeLauncherService();
        var viewModel = CreateViewModel(service, new RecordingLogger());

        await viewModel.PrepareSaveCommand.ExecuteAsync(null);

        Assert.Empty(viewModel.SaveProfiles);
        Assert.Contains("no", viewModel.Status, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("profile", viewModel.Status, StringComparison.OrdinalIgnoreCase);
        Assert.Empty(service.PrepareCalls);
        Assert.False(viewModel.CanLaunch);
    }

    [Fact]
    public async Task PrepareSaveCommand_OneProfileDisplaysExactSourceAndDestinationBeforeFirstCopy()
    {
        var source = Path.Combine("C:\\Documents", SteamId, "DRAKS0005.sl2");
        var service = new FakeLauncherService
        {
            SaveProfiles = [new SaveProfileCandidate(SteamId, source)],
            PrepareResult = DedicatedSaveResult.Fail(
                SaveErrorCode.FirstCopyConfirmationRequired,
                "First-copy confirmation is required.")
        };
        var local = Path.Combine("C:\\Local", "DSR-Randomizer");
        var viewModel = new MainWindowViewModel(service, new RecordingLogger(), local);

        await viewModel.PrepareSaveCommand.ExecuteAsync(null);

        Assert.Equal(SteamId, viewModel.SelectedSaveProfile?.SteamId);
        Assert.Equal(source, viewModel.SelectedSaveSourcePath);
        Assert.Equal(
            Path.Combine(local, "saves", SteamId, "DRAKS0005.rmm"),
            viewModel.DedicatedSavePath);
        Assert.Contains(source, viewModel.Status, StringComparison.Ordinal);
        Assert.Contains(viewModel.DedicatedSavePath, viewModel.Status, StringComparison.Ordinal);
        Assert.Equal((SteamId, false), Assert.Single(service.PrepareCalls));
    }

    [Fact]
    public async Task PrepareSaveCommand_MultipleProfilesRequiresExplicitSelection()
    {
        var first = new SaveProfileCandidate(
            "12345678901234567",
            @"C:\Documents\12345678901234567\DRAKS0005.sl2");
        var second = new SaveProfileCandidate(
            "76543210987654321",
            @"C:\Documents\76543210987654321\DRAKS0005.sl2");
        var service = new FakeLauncherService { SaveProfiles = [first, second] };
        var viewModel = CreateViewModel(service, new RecordingLogger());

        await viewModel.PrepareSaveCommand.ExecuteAsync(null);

        Assert.Equal(2, viewModel.SaveProfiles.Count);
        Assert.Null(viewModel.SelectedSaveProfile);
        Assert.Contains("select", viewModel.Status, StringComparison.OrdinalIgnoreCase);
        Assert.Empty(service.PrepareCalls);
    }

    [Fact]
    public async Task PrepareSaveCommand_SelectedProfileAndConfirmationPrepareExactSteamId()
    {
        var first = new SaveProfileCandidate(
            "12345678901234567",
            @"C:\Documents\12345678901234567\DRAKS0005.sl2");
        var second = new SaveProfileCandidate(
            "76543210987654321",
            @"C:\Documents\76543210987654321\DRAKS0005.sl2");
        var service = new FakeLauncherService
        {
            SaveProfiles = [first, second],
            PrepareResult = new DedicatedSaveResult(
                true,
                false,
                @"C:\Local\saves\76543210987654321\DRAKS0005.rmm",
                SaveErrorCode.None,
                string.Empty)
        };
        var viewModel = CreateViewModel(service, new RecordingLogger());
        await viewModel.PrepareSaveCommand.ExecuteAsync(null);
        viewModel.SelectedSaveProfile = second;
        viewModel.FirstCopyConfirmed = true;

        await viewModel.PrepareSaveCommand.ExecuteAsync(null);

        Assert.Equal((second.SteamId, true), Assert.Single(service.PrepareCalls));
        Assert.Contains("created", viewModel.Status, StringComparison.OrdinalIgnoreCase);
        Assert.False(viewModel.CanLaunch);
    }

    [Fact]
    public async Task PrepareSaveCommand_ExistingRmmReportsReuseAndKeepsLaunchDisabled()
    {
        var source = new SaveProfileCandidate(
            SteamId,
            @"C:\Documents\12345678901234567\DRAKS0005.sl2");
        var destination = @"C:\Local\saves\12345678901234567\DRAKS0005.rmm";
        var service = new FakeLauncherService
        {
            SaveProfiles = [source],
            PrepareResult = new DedicatedSaveResult(
                true,
                true,
                destination,
                SaveErrorCode.None,
                string.Empty)
        };
        var viewModel = CreateViewModel(service, new RecordingLogger());

        await viewModel.PrepareSaveCommand.ExecuteAsync(null);

        Assert.Contains("existing DRAKS0005.rmm", viewModel.Status, StringComparison.OrdinalIgnoreCase);
        Assert.Equal((SteamId, false), Assert.Single(service.PrepareCalls));
        Assert.False(viewModel.CanLaunch);
    }

    [Fact]
    public async Task PrepareSaveCommand_InFlightProfileChangeRendersOriginalSelectionSnapshot()
    {
        var first = new SaveProfileCandidate(
            "12345678901234567",
            @"C:\Documents\12345678901234567\DRAKS0005.sl2");
        var second = new SaveProfileCandidate(
            "76543210987654321",
            @"C:\Documents\76543210987654321\DRAKS0005.sl2");
        var pendingResult = new TaskCompletionSource<DedicatedSaveResult>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var service = new FakeLauncherService
        {
            SaveProfiles = [first, second],
            PendingPrepareResult = pendingResult
        };
        var local = @"C:\Local\DSR-Randomizer";
        var viewModel = new MainWindowViewModel(service, new RecordingLogger(), local);
        await viewModel.PrepareSaveCommand.ExecuteAsync(null);
        viewModel.SelectedSaveProfile = first;

        var preparation = viewModel.PrepareSaveCommand.ExecuteAsync(null);
        await service.PrepareEntered.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.True(viewModel.IsBusy);
        Assert.False(viewModel.AreSaveControlsEnabled);
        viewModel.SelectedSaveProfile = second;
        viewModel.FirstCopyConfirmed = true;
        pendingResult.SetResult(DedicatedSaveResult.Fail(
            SaveErrorCode.FirstCopyConfirmationRequired,
            "First-copy confirmation is required."));
        await preparation;

        Assert.Equal((first.SteamId, false), Assert.Single(service.PrepareCalls));
        Assert.Contains(first.SourcePath, viewModel.Status, StringComparison.Ordinal);
        Assert.Contains(
            Path.Combine(local, "saves", first.SteamId, "DRAKS0005.rmm"),
            viewModel.Status,
            StringComparison.Ordinal);
        Assert.DoesNotContain(second.SourcePath, viewModel.Status, StringComparison.Ordinal);
        Assert.False(viewModel.IsBusy);
        Assert.True(viewModel.AreSaveControlsEnabled);
    }

    private static MainWindowViewModel CreateViewModel(
        ILauncherService service,
        IExternalLogger logger) =>
        new(service, logger, Path.Combine(Path.GetTempPath(), "DSR-Randomizer-Launcher-Tests"));

    private sealed class FakeLauncherService : ILauncherService
    {
        public IReadOnlyList<SaveProfileCandidate> SaveProfiles { get; init; } = [];

        public DedicatedSaveResult PrepareResult { get; init; } =
            DedicatedSaveResult.Fail(SaveErrorCode.SourceMissing, "not configured");

        public List<(string SteamId, bool FirstCopyConfirmed)> PrepareCalls { get; } = [];

        public TaskCompletionSource<DedicatedSaveResult>? PendingPrepareResult { get; init; }

        public TaskCompletionSource PrepareEntered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public VerificationResult VerificationResult { get; init; } = new(
            true,
            @"C:\Steam\DSR",
            new GameFileCatalog(Array.Empty<GameFileEntry>(), 0),
            Array.Empty<string>());

        public Exception? RuntimeException { get; init; }

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

        public Task<IReadOnlyList<SaveProfileCandidate>> DiscoverSaveProfilesAsync(
            CancellationToken cancellationToken) =>
            Task.FromResult(SaveProfiles);

        public Task<DedicatedSaveResult> PrepareDedicatedSaveAsync(
            string steamId,
            bool firstCopyConfirmed,
            CancellationToken cancellationToken)
        {
            PrepareCalls.Add((steamId, firstCopyConfirmed));
            PrepareEntered.TrySetResult();
            return PendingPrepareResult?.Task ?? Task.FromResult(PrepareResult);
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
