using DSRRandomizer.Foundation.Installation;
using DSRRandomizer.Foundation.Runtime;
using DSRRandomizer.Foundation.Saves;
using DSRRandomizer.Launcher.Logging;
using DSRRandomizer.Launcher.Services;
using DSRRandomizer.Launcher.ViewModels;

namespace DSRRandomizer.Launcher.Tests.ViewModels;

public sealed class MainWindowViewModelTests
{
    private const string SteamId = "12345678901234567";

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
                SaveErrorCode.MultipleProfilesRequireSelection,
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
            return Task.FromResult(PrepareResult);
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
}
