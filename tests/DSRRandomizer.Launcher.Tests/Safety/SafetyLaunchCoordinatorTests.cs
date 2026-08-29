using DSRRandomizer.Foundation.Safety;
using DSRRandomizer.Launcher.Safety;

namespace DSRRandomizer.Launcher.Tests.Safety;

public sealed class SafetyLaunchCoordinatorTests
{
    [Theory]
    [InlineData(FailurePoint.Create, 0)]
    [InlineData(FailurePoint.AssignJob, 1)]
    [InlineData(FailurePoint.Inject, 1)]
    [InlineData(FailurePoint.Handshake, 1)]
    public async Task LaunchAsync_NeverResumesAfterProtectionFailure(
        FailurePoint failurePoint,
        int expectedTerminateCalls)
    {
        var platform = new RecordingPlatform(failurePoint);

        var result = await new SafetyLaunchCoordinator(platform)
            .LaunchAsync(CreateRequest(), CancellationToken.None);

        Assert.False(result.Started);
        Assert.Equal(0, platform.Process.ResumeCalls);
        Assert.Equal(expectedTerminateCalls, platform.Process.TerminateCalls);
        Assert.Equal(0, platform.Process.WaitForExitCalls);
    }

    [Fact]
    public async Task LaunchAsync_CompleteHandshakeResumesOnceAndWaitsForExit()
    {
        var platform = new RecordingPlatform(FailurePoint.None);

        var result = await new SafetyLaunchCoordinator(platform)
            .LaunchAsync(CreateRequest(), CancellationToken.None);

        Assert.True(result.Started);
        Assert.Equal(string.Empty, result.ErrorCode);
        Assert.Equal(1, platform.Process.AssignJobCalls);
        Assert.Equal(1, platform.Process.InjectCalls);
        Assert.Equal(1, platform.Process.ResumeCalls);
        Assert.Equal(1, platform.Process.WaitForExitCalls);
        Assert.Equal(0, platform.Process.TerminateCalls);
        Assert.Equal(0, result.ExitCode);
    }

    [Fact]
    public async Task LaunchAsync_ExactSimplifiedBitmap_ResumesWithoutMonitorSession()
    {
        var platform = new RecordingPlatform(
            new ProtectionHandshake(true, 0x7F, string.Empty, Session: null));

        var result = await new SafetyLaunchCoordinator(platform)
            .LaunchAsync(CreateRequest(requiredFlags: 0x7F), CancellationToken.None);

        Assert.True(result.Started);
        Assert.Equal(1, platform.Process.ResumeCalls);
        Assert.Equal(0, platform.Process.TerminateCalls);
    }

    [Fact]
    public async Task LaunchAsync_ExactDedicatedSaveBitmap_ResumesWithoutMonitorSession()
    {
        var platform = new RecordingPlatform(
            new ProtectionHandshake(true, 0x201, string.Empty, Session: null));

        var result = await new SafetyLaunchCoordinator(platform)
            .LaunchAsync(CreateRequest(requiredFlags: 0x201), CancellationToken.None);

        Assert.True(result.Started);
        Assert.Equal(1, platform.Process.ResumeCalls);
        Assert.Equal(0, platform.Process.TerminateCalls);
    }

    [Theory]
    [InlineData(0x3UL)]
    [InlineData(0x203UL)]
    [InlineData(0x10000000201UL)]
    public async Task LaunchAsync_NonExactDedicatedSaveBitmap_NeverResumes(ulong flags)
    {
        var platform = new RecordingPlatform(
            new ProtectionHandshake(true, flags, string.Empty, Session: null));

        var result = await new SafetyLaunchCoordinator(platform)
            .LaunchAsync(CreateRequest(requiredFlags: 0x201), CancellationToken.None);

        Assert.False(result.Started);
        Assert.Equal(0, platform.Process.ResumeCalls);
        Assert.Equal(1, platform.Process.TerminateCalls);
    }

    [Fact]
    public async Task LaunchAsync_ExactDedicatedSaveBitmapWithMonitorSession_NeverResumes()
    {
        var platform = new RecordingPlatform(
            new ProtectionHandshake(
                true,
                0x201,
                string.Empty,
                new BlockingProtectionSession()));

        var result = await new SafetyLaunchCoordinator(platform)
            .LaunchAsync(CreateRequest(requiredFlags: 0x201), CancellationToken.None);

        Assert.False(result.Started);
        Assert.Equal(0, platform.Process.ResumeCalls);
        Assert.Equal(1, platform.Process.TerminateCalls);
    }

    [Theory]
    [InlineData(0x7EUL)]
    [InlineData(0xFFUL)]
    [InlineData(0x17FUL)]
    public async Task LaunchAsync_NonExactSimplifiedBitmap_NeverResumes(ulong flags)
    {
        var platform = new RecordingPlatform(
            new ProtectionHandshake(true, flags, string.Empty, Session: null));

        var result = await new SafetyLaunchCoordinator(platform)
            .LaunchAsync(CreateRequest(requiredFlags: 0x7F), CancellationToken.None);

        Assert.False(result.Started);
        Assert.Equal(0, platform.Process.ResumeCalls);
        Assert.Equal(1, platform.Process.TerminateCalls);
    }

    [Fact]
    public async Task LaunchAsync_ExactSimplifiedBitmapWithMonitorSession_NeverResumes()
    {
        var platform = new RecordingPlatform(
            new ProtectionHandshake(
                true,
                0x7F,
                string.Empty,
                new BlockingProtectionSession()));

        var result = await new SafetyLaunchCoordinator(platform)
            .LaunchAsync(CreateRequest(requiredFlags: 0x7F), CancellationToken.None);

        Assert.False(result.Started);
        Assert.Equal(0, platform.Process.ResumeCalls);
        Assert.Equal(1, platform.Process.TerminateCalls);
    }

    [Fact]
    public async Task LaunchAsync_UnexpectedPreviousSuspendCountTerminatesWithoutWaiting()
    {
        var platform = new RecordingPlatform(FailurePoint.ResumeCount);

        var result = await new SafetyLaunchCoordinator(platform)
            .LaunchAsync(CreateRequest(), CancellationToken.None);

        Assert.False(result.Started);
        Assert.Equal("SAFETY_RESUME_COUNT_INVALID", result.ErrorCode);
        Assert.Equal(1, platform.Process.ResumeCalls);
        Assert.Equal(1, platform.Process.TerminateCalls);
        Assert.Equal(0, platform.Process.WaitForExitCalls);
    }

    [Fact]
    public async Task LaunchAsync_IncompleteProtectionFlagsTerminatesBeforeResume()
    {
        var platform = new RecordingPlatform(FailurePoint.IncompleteFlags);

        var result = await new SafetyLaunchCoordinator(platform)
            .LaunchAsync(CreateRequest(), CancellationToken.None);

        Assert.False(result.Started);
        Assert.Equal("SAFETY_FLAGS_INCOMPLETE", result.ErrorCode);
        Assert.Equal(0, platform.Process.ResumeCalls);
        Assert.Equal(1, platform.Process.TerminateCalls);
        Assert.Equal(0, platform.Process.WaitForExitCalls);
    }

    [Fact]
    public async Task LaunchAsync_UnexpectedPlatformFailureTerminatesAndReturnsStableError()
    {
        var platform = new RecordingPlatform(FailurePoint.UnexpectedInject);

        var result = await new SafetyLaunchCoordinator(platform)
            .LaunchAsync(CreateRequest(), CancellationToken.None);

        Assert.False(result.Started);
        Assert.Equal("SAFETY_UNEXPECTED_FAILURE", result.ErrorCode);
        Assert.Equal(0, platform.Process.ResumeCalls);
        Assert.Equal(1, platform.Process.TerminateCalls);
        Assert.Equal(0, platform.Process.WaitForExitCalls);
    }

    [Fact]
    public async Task LaunchAsync_CancellationAfterCreationTerminatesAndRethrows()
    {
        using var cancellation = new CancellationTokenSource();
        var platform = new RecordingPlatform(FailurePoint.WaitForExit, cancellation);

        await Assert.ThrowsAsync<OperationCanceledException>(
            () => new SafetyLaunchCoordinator(platform)
                .LaunchAsync(CreateRequest(), cancellation.Token));

        Assert.Equal(1, platform.Process.ResumeCalls);
        Assert.Equal(1, platform.Process.TerminateCalls);
    }

    private static SafetyLaunchRequest CreateRequest(ulong requiredFlags = 0x181) => new(
        @"C:\Local\runtime\DSRRandomizer.SuspendedFixture.exe",
        @"C:\Local\runtime",
        @"C:\Local\components\DSRRandomizer.Runtime.dll",
        CompatibilityProfileCatalog.Default.Select(new ExecutableIdentity(
            50286344,
            "a45aaa36dd2f6cc151670a639ea5547043cf38ea79ff4178b963c6ed71f98d7b",
            0x8664,
            0x6344ca56,
            52015104)),
        RequiredProtectionFlags: requiredFlags,
        DiagnosticMode: true);

    public enum FailurePoint
    {
        None,
        Create,
        AssignJob,
        Inject,
        Handshake,
        IncompleteFlags,
        UnexpectedInject,
        ResumeCount,
        WaitForExit
    }

    private sealed class RecordingPlatform : IProtectedProcessPlatform
    {
        private readonly FailurePoint _failurePoint;

        public RecordingPlatform(
            FailurePoint failurePoint,
            CancellationTokenSource? cancellation = null)
        {
            _failurePoint = failurePoint;
            Process = new RecordingProcess(failurePoint, cancellation);
        }

        public RecordingPlatform(ProtectionHandshake handshake)
        {
            _failurePoint = FailurePoint.None;
            Process = new RecordingProcess(
                FailurePoint.None,
                cancellation: null,
                handshake);
        }

        public RecordingProcess Process { get; }

        public Task<IProtectedProcess> CreateSuspendedAsync(
            SafetyLaunchRequest request,
            CancellationToken cancellationToken)
        {
            if (_failurePoint == FailurePoint.Create)
            {
                throw new SafetyLaunchException("SAFETY_CREATE_FAILED");
            }

            Process.RequiredFlags = request.RequiredProtectionFlags;
            return Task.FromResult<IProtectedProcess>(Process);
        }
    }

    private sealed class RecordingProcess(
        FailurePoint failurePoint,
        CancellationTokenSource? cancellation,
        ProtectionHandshake? handshake = null) : IProtectedProcess
    {
        public int ProcessId => 42;

        public int AssignJobCalls { get; private set; }

        public int InjectCalls { get; private set; }

        public int ResumeCalls { get; private set; }

        public int TerminateCalls { get; private set; }

        public int WaitForExitCalls { get; private set; }

        public ulong RequiredFlags { get; set; }

        public void AssignKillOnCloseJob()
        {
            AssignJobCalls++;
            if (failurePoint == FailurePoint.AssignJob)
            {
                throw new SafetyLaunchException("SAFETY_JOB_ASSIGN_FAILED");
            }
        }

        public Task<ProtectionHandshake> InjectAndInitializeAsync(CancellationToken cancellationToken)
        {
            InjectCalls++;
            if (failurePoint == FailurePoint.Inject)
            {
                throw new SafetyLaunchException("SAFETY_INJECTION_FAILED");
            }

            if (failurePoint == FailurePoint.UnexpectedInject)
            {
                throw new IOException("Simulated unexpected platform failure.");
            }

            if (handshake is not null)
            {
                return Task.FromResult(handshake);
            }

            return Task.FromResult(failurePoint == FailurePoint.Handshake
                ? ProtectionHandshake.Failed("SAFETY_HANDSHAKE_FAILED")
                : new ProtectionHandshake(
                    true,
                    failurePoint == FailurePoint.IncompleteFlags
                        ? RequiredFlags & ~1UL
                        : RequiredFlags,
                    string.Empty,
                    new BlockingProtectionSession()));
        }

        public uint ResumeMainThread()
        {
            ResumeCalls++;
            return failurePoint == FailurePoint.ResumeCount ? 2u : 1u;
        }

        public void TerminateJob() => TerminateCalls++;

        public Task<int> WaitForExitAsync(CancellationToken cancellationToken)
        {
            WaitForExitCalls++;
            if (failurePoint == FailurePoint.WaitForExit)
            {
                cancellation?.Cancel();
                cancellationToken.ThrowIfCancellationRequested();
            }

            return Task.FromResult(0);
        }

        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
    }

    private sealed class BlockingProtectionSession : IProtectionSession
    {
        public async Task<ProtectionMonitorResult> MonitorAsync(
            ulong expectedActiveFlags,
            CancellationToken cancellationToken)
        {
            await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
            throw new InvalidOperationException("The protection monitor completed unexpectedly.");
        }

        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
    }
}
