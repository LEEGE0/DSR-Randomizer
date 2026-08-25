using System.Buffers.Binary;
using System.Threading.Channels;
using DSRRandomizer.Foundation.Safety;
using DSRRandomizer.Launcher.Native;
using DSRRandomizer.Launcher.Safety;

namespace DSRRandomizer.Launcher.Tests.Safety;

public sealed class ProtectionHeartbeatTests
{
    private const ulong ExpectedFlags = 0x183;

    [Fact]
    public async Task FiveConsecutiveLocalTicksWithoutHeartbeat_ReportTimeout()
    {
        var harness = new MonitorHarness();
        var monitorTask = harness.MonitorAsync();

        for (var tick = 0; tick < 4; tick++)
        {
            harness.AdvanceOneSecond();
            await harness.DrainAsync();
            Assert.False(monitorTask.IsCompleted);
        }

        harness.AdvanceOneSecond();
        var result = await monitorTask;

        Assert.Equal("HEARTBEAT_TIMEOUT", result.ErrorCode);
    }

    [Fact]
    public async Task LocallyReceivedHeartbeat_ResetsConsecutiveMissCount()
    {
        var harness = new MonitorHarness();
        var monitorTask = harness.MonitorAsync();

        for (var tick = 0; tick < 4; tick++)
        {
            harness.AdvanceOneSecond();
            await harness.DrainAsync();
        }
        harness.SendHeartbeat(1, 1_000, Counters(1));
        await harness.DrainAsync();
        harness.AdvanceOneSecond();
        await harness.DrainAsync();
        for (var tick = 0; tick < 4; tick++)
        {
            harness.AdvanceOneSecond();
            await harness.DrainAsync();
            Assert.False(monitorTask.IsCompleted);
        }

        harness.AdvanceOneSecond();
        Assert.Equal("HEARTBEAT_TIMEOUT", (await monitorTask).ErrorCode);
    }

    [Theory]
    [InlineData(1U, "HOOK_INTEGRITY_FAILED")]
    [InlineData(2U, "HEARTBEAT_STOPPED")]
    [InlineData(3U, "PROTECTION_THREAD_FAILED")]
    public async Task AuthenticatedFatalEvent_ReportsExactStableCode(
        uint fatalCode,
        string expectedErrorCode)
    {
        var harness = new MonitorHarness();
        var monitorTask = harness.MonitorAsync();

        harness.SendFatal((ProtectionFatalCode)fatalCode);

        Assert.Equal(expectedErrorCode, (await monitorTask).ErrorCode);
    }

    [Theory]
    [InlineData(1UL, 1_000UL, 1UL, 2_000UL, "SAFETY_HEARTBEAT_SEQUENCE_INVALID")]
    [InlineData(1UL, 2_000UL, 2UL, 2_000UL, "SAFETY_HEARTBEAT_CLOCK_INVALID")]
    public async Task ReplayOrNonIncreasingNativeTimestamp_IsRejected(
        ulong firstSequence,
        ulong firstTimestamp,
        ulong secondSequence,
        ulong secondTimestamp,
        string expectedErrorCode)
    {
        var harness = new MonitorHarness();
        var monitorTask = harness.MonitorAsync();
        harness.SendHeartbeat(firstSequence, firstTimestamp, Counters(2));
        harness.SendHeartbeat(secondSequence, secondTimestamp, Counters(3));

        Assert.Equal(expectedErrorCode, (await monitorTask).ErrorCode);
    }

    [Fact]
    public async Task ActiveFlagDrift_IsRejected()
    {
        var harness = new MonitorHarness();
        var monitorTask = harness.MonitorAsync();

        harness.SendHeartbeat(1, 1_000, Counters(0), ExpectedFlags ^ 0x80);

        Assert.Equal("SAFETY_FLAGS_CHANGED", (await monitorTask).ErrorCode);
    }

    [Fact]
    public async Task AnyDeniedCounterRollback_IsRejected()
    {
        var harness = new MonitorHarness();
        var monitorTask = harness.MonitorAsync();
        harness.SendHeartbeat(1, 1_000, [4, 5, 6, 7, 8, 9]);
        harness.SendHeartbeat(2, 2_000, [4, 5, 6, 7, 7, 9]);

        Assert.Equal("SAFETY_DENIED_COUNTER_ROLLBACK", (await monitorTask).ErrorCode);
    }

    [Theory]
    [InlineData(FrameMutation.Size, "SAFETY_IPC_PROTOCOL_INVALID")]
    [InlineData(FrameMutation.Kind, "SAFETY_IPC_PROTOCOL_INVALID")]
    [InlineData(FrameMutation.Version, "SAFETY_IPC_PROTOCOL_INVALID")]
    [InlineData(FrameMutation.Nonce, "SAFETY_IPC_AUTH_FAILED")]
    [InlineData(FrameMutation.TrailingByte, "SAFETY_IPC_PROTOCOL_INVALID")]
    public void EveryHeartbeatFrameField_IsValidatedExactly(
        FrameMutation mutation,
        string expectedErrorCode)
    {
        var nonce = Enumerable.Range(0, 32).Select(value => (byte)value).ToArray();
        var frame = CreateHeartbeatFrame(nonce);
        frame = mutation switch
        {
            FrameMutation.Size => MutateUInt16(frame, 6, checked((ushort)(frame.Length - 1))),
            FrameMutation.Kind => MutateUInt32(frame, 40, 99),
            FrameMutation.Version => MutateUInt16(frame, 4, 3),
            FrameMutation.Nonce => MutateByte(frame, 8, 0xff),
            FrameMutation.TrailingByte => [.. frame, 0],
            _ => frame
        };

        var result = ProtectionPipeProtocol.ParseFrame(frame, nonce);

        Assert.False(result.Success);
        Assert.Equal(expectedErrorCode, result.ErrorCode);
    }

    [Theory]
    [InlineData("HOOK_INTEGRITY_FAILED")]
    [InlineData("PROTECTION_THREAD_FAILED")]
    [InlineData("HEARTBEAT_TIMEOUT")]
    public async Task FatalMonitorResult_TerminatesFixtureJobExactlyOnce(
        string errorCode)
    {
        var session = new RecordingSession(
            ProtectionMonitorResult.Failed(errorCode));
        var platform = new MonitoringPlatform(session, completeNaturally: false);

        var result = await new SafetyLaunchCoordinator(platform)
            .LaunchAsync(CreateRequest(), CancellationToken.None);

        Assert.False(result.Started);
        Assert.Equal(errorCode, result.ErrorCode);
        Assert.Equal(1, platform.Process.TerminateCalls);
        Assert.Equal(1, session.DisposeCalls);
    }

    [Fact]
    public async Task NaturalProcessExit_CancelsAndDisposesMonitorWithoutFalseFatal()
    {
        var session = new RecordingSession(result: null);
        var platform = new MonitoringPlatform(session, completeNaturally: true);

        var result = await new SafetyLaunchCoordinator(platform)
            .LaunchAsync(CreateRequest(), CancellationToken.None);

        Assert.True(result.Started, result.ErrorCode);
        Assert.Equal(0, result.ExitCode);
        Assert.Equal(0, platform.Process.TerminateCalls);
        Assert.Equal(1, session.CancellationCalls);
        Assert.Equal(1, session.DisposeCalls);
    }

    [Fact]
    public async Task PipeEofImmediatelyBeforeNaturalExit_DoesNotCreateFalseFatal()
    {
        var session = new RecordingSession(
            ProtectionMonitorResult.Failed("SAFETY_IPC_CLOSED"));
        var platform = new MonitoringPlatform(
            session,
            completeNaturally: false,
            naturalExitDelay: TimeSpan.FromMilliseconds(10));

        var result = await new SafetyLaunchCoordinator(platform)
            .LaunchAsync(CreateRequest(), CancellationToken.None);

        Assert.True(result.Started, result.ErrorCode);
        Assert.Equal(0, platform.Process.TerminateCalls);
    }

    private static ulong[] Counters(ulong value) =>
        [value, value, value, value, value, value];

    private static SafetyLaunchRequest CreateRequest() => new(
        @"C:\Local\runtime\DSRRandomizer.SuspendedFixture.exe",
        @"C:\Local\runtime",
        @"C:\Local\components\DSRRandomizer.Runtime.dll",
        CompatibilityProfileCatalog.Default.Select(new ExecutableIdentity(
            50286344,
            "a45aaa36dd2f6cc151670a639ea5547043cf38ea79ff4178b963c6ed71f98d7b",
            0x8664,
            0x6344ca56,
            52015104)),
        ExpectedFlags,
        DiagnosticMode: true);

    private static byte[] CreateHeartbeatFrame(byte[] nonce)
    {
        var frame = new byte[116];
        BinaryPrimitives.WriteUInt32LittleEndian(frame, 0x44535252);
        BinaryPrimitives.WriteUInt16LittleEndian(frame.AsSpan(4), 2);
        BinaryPrimitives.WriteUInt16LittleEndian(frame.AsSpan(6), 116);
        nonce.CopyTo(frame, 8);
        BinaryPrimitives.WriteUInt32LittleEndian(frame.AsSpan(40), 2);
        BinaryPrimitives.WriteUInt64LittleEndian(frame.AsSpan(44), 1);
        BinaryPrimitives.WriteUInt64LittleEndian(frame.AsSpan(52), 1_000);
        BinaryPrimitives.WriteUInt64LittleEndian(frame.AsSpan(60), ExpectedFlags);
        return frame;
    }

    private static byte[] MutateUInt16(byte[] source, int offset, ushort value)
    {
        var clone = (byte[])source.Clone();
        BinaryPrimitives.WriteUInt16LittleEndian(clone.AsSpan(offset), value);
        return clone;
    }

    private static byte[] MutateUInt32(byte[] source, int offset, uint value)
    {
        var clone = (byte[])source.Clone();
        BinaryPrimitives.WriteUInt32LittleEndian(clone.AsSpan(offset), value);
        return clone;
    }

    private static byte[] MutateByte(byte[] source, int offset, byte value)
    {
        var clone = (byte[])source.Clone();
        clone[offset] = value;
        return clone;
    }

    public enum FrameMutation
    {
        Size,
        Kind,
        Version,
        Nonce,
        TrailingByte
    }

    private sealed class MonitorHarness : IProtectionMessageSource, IProtectionTickSource
    {
        private readonly Channel<ProtectionChannelMessage> _messages = Channel.CreateUnbounded<ProtectionChannelMessage>();
        private readonly Channel<bool> _ticks = Channel.CreateUnbounded<bool>();

        public Task<ProtectionMonitorResult> MonitorAsync() =>
            new ProtectionHeartbeatMonitor(this, this).MonitorAsync(
                ExpectedFlags,
                CancellationToken.None);

        public void SendHeartbeat(
            ulong sequence,
            ulong timestamp,
            IReadOnlyList<ulong> counters,
            ulong activeFlags = ExpectedFlags) =>
            _messages.Writer.TryWrite(ProtectionChannelMessage.Heartbeat(
                sequence,
                timestamp,
                activeFlags,
                counters));

        public void SendFatal(ProtectionFatalCode fatalCode) =>
            _messages.Writer.TryWrite(ProtectionChannelMessage.Fatal(fatalCode));

        public void AdvanceOneSecond() => _ticks.Writer.TryWrite(true);

        public async Task DrainAsync()
        {
            for (var attempt = 0; attempt < 10; attempt++)
            {
                await Task.Yield();
            }
        }

        public ValueTask<ProtectionChannelMessage> ReadAsync(CancellationToken cancellationToken) =>
            _messages.Reader.ReadAsync(cancellationToken);

        public ValueTask<bool> WaitForNextTickAsync(CancellationToken cancellationToken) =>
            _ticks.Reader.ReadAsync(cancellationToken);

        public ValueTask DisposeAsync()
        {
            _messages.Writer.TryComplete();
            _ticks.Writer.TryComplete();
            return ValueTask.CompletedTask;
        }
    }

    private sealed class RecordingSession(ProtectionMonitorResult? result) : IProtectionSession
    {
        public int CancellationCalls { get; private set; }

        public int DisposeCalls { get; private set; }

        public async Task<ProtectionMonitorResult> MonitorAsync(
            ulong expectedActiveFlags,
            CancellationToken cancellationToken)
        {
            if (result is not null)
            {
                return result;
            }

            try
            {
                await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
                throw new InvalidOperationException("Infinite delay completed without cancellation.");
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                CancellationCalls++;
                throw;
            }
        }

        public ValueTask DisposeAsync()
        {
            DisposeCalls++;
            return ValueTask.CompletedTask;
        }
    }

    private sealed class MonitoringPlatform(
        RecordingSession session,
        bool completeNaturally,
        TimeSpan? naturalExitDelay = null) : IProtectedProcessPlatform
    {
        public RecordingProcess Process { get; } = new(
            session,
            completeNaturally,
            naturalExitDelay);

        public Task<IProtectedProcess> CreateSuspendedAsync(
            SafetyLaunchRequest request,
            CancellationToken cancellationToken) =>
            Task.FromResult<IProtectedProcess>(Process);
    }

    private sealed class RecordingProcess(
        IProtectionSession session,
        bool completeNaturally,
        TimeSpan? naturalExitDelay) : IProtectedProcess
    {
        private readonly TaskCompletionSource<int> _exit =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public int ProcessId => 42;

        public int TerminateCalls { get; private set; }

        public void AssignKillOnCloseJob()
        {
        }

        public Task<ProtectionHandshake> InjectAndInitializeAsync(
            CancellationToken cancellationToken) =>
            Task.FromResult(new ProtectionHandshake(
                true,
                ExpectedFlags,
                string.Empty,
                session));

        public uint ResumeMainThread()
        {
            if (completeNaturally)
            {
                _exit.TrySetResult(0);
            }
            else if (naturalExitDelay is not null)
            {
                _ = CompleteNaturalExitAsync(naturalExitDelay.Value);
            }
            return 1;
        }

        public void TerminateJob()
        {
            TerminateCalls++;
            _exit.TrySetResult(1);
        }

        public Task<int> WaitForExitAsync(CancellationToken cancellationToken) =>
            _exit.Task.WaitAsync(cancellationToken);

        public ValueTask DisposeAsync() => ValueTask.CompletedTask;

        private async Task CompleteNaturalExitAsync(TimeSpan delay)
        {
            await Task.Delay(delay);
            _exit.TrySetResult(0);
        }
    }
}
