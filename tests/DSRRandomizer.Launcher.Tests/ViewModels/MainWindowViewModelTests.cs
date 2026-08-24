using DSRRandomizer.Foundation.Installation;
using DSRRandomizer.Foundation.Runtime;
using DSRRandomizer.Launcher.Logging;
using DSRRandomizer.Launcher.Services;
using DSRRandomizer.Launcher.ViewModels;

namespace DSRRandomizer.Launcher.Tests.ViewModels;

public sealed class MainWindowViewModelTests
{
    [Fact]
    public async Task InitializeCommand_NativeFoundationStillKeepsPublicLaunchLocked()
    {
        var service = new FakeLauncherService();
        var viewModel = new MainWindowViewModel(service, new RecordingLogger())
        {
            GamePath = @"C:\Steam\DSR"
        };

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
        var viewModel = new MainWindowViewModel(service, logger)
        {
            GamePath = @"C:\Steam\DSR"
        };

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
        var viewModel = new MainWindowViewModel(service, new RecordingLogger())
        {
            GamePath = @"C:\Broken"
        };

        await viewModel.VerifyCommand.ExecuteAsync(null);

        Assert.Equal("Installation verification failed: missing executable; missing sound", viewModel.Status);
        Assert.False(viewModel.CanLaunch);
    }

    private sealed class FakeLauncherService : ILauncherService
    {
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
