using System.Threading.Channels;
using DSRRandomizer.Launcher.Safety;

namespace DSRRandomizer.Launcher.Native;

internal enum ProtectionMessageKind : uint
{
    Handshake = 1,
    Heartbeat = 2,
    Fatal = 3
}

internal enum ProtectionFatalCode : uint
{
    HookIntegrityFailed = 1,
    HeartbeatStopped = 2,
    ProtectionThreadFailed = 3
}

internal sealed record ProtectionChannelMessage(
    ProtectionMessageKind Kind,
    uint Status,
    ulong Sequence,
    ulong MonotonicMilliseconds,
    ulong ActiveFlags,
    IReadOnlyList<ulong> DeniedCounters,
    ProtectionFatalCode FatalCode)
{
    public static ProtectionChannelMessage Handshake(uint status, ulong activeFlags) =>
        new(
            ProtectionMessageKind.Handshake,
            status,
            0,
            0,
            activeFlags,
            Array.Empty<ulong>(),
            0);

    public static ProtectionChannelMessage Heartbeat(
        ulong sequence,
        ulong monotonicMilliseconds,
        ulong activeFlags,
        IReadOnlyList<ulong> deniedCounters) =>
        new(
            ProtectionMessageKind.Heartbeat,
            0,
            sequence,
            monotonicMilliseconds,
            activeFlags,
            deniedCounters.ToArray(),
            0);

    public static ProtectionChannelMessage Fatal(ProtectionFatalCode fatalCode) =>
        new(
            ProtectionMessageKind.Fatal,
            0,
            0,
            0,
            0,
            Array.Empty<ulong>(),
            fatalCode);
}

internal interface IProtectionMessageSource
{
    ValueTask<ProtectionChannelMessage> ReadAsync(CancellationToken cancellationToken);
}

internal interface IProtectionTickSource : IAsyncDisposable
{
    ValueTask<bool> WaitForNextTickAsync(CancellationToken cancellationToken);
}

internal sealed class PeriodicProtectionTickSource : IProtectionTickSource
{
    private readonly PeriodicTimer _timer = new(TimeSpan.FromSeconds(1));

    public ValueTask<bool> WaitForNextTickAsync(CancellationToken cancellationToken) =>
        _timer.WaitForNextTickAsync(cancellationToken);

    public ValueTask DisposeAsync()
    {
        _timer.Dispose();
        return ValueTask.CompletedTask;
    }
}

internal sealed class ProtectionHeartbeatMonitor(
    IProtectionMessageSource messages,
    IProtectionTickSource ticks)
{
    private const int DeniedCounterCount = 6;
    private const int MaximumConsecutiveMisses = 5;

    public async Task<ProtectionMonitorResult> MonitorAsync(
        ulong expectedActiveFlags,
        CancellationToken cancellationToken)
    {
        await using var ownedTicks = ticks;
        var lastSequence = 0UL;
        var lastMonotonicMilliseconds = 0UL;
        var lastCounters = new ulong[DeniedCounterCount];
        var receivedSinceTick = false;
        var consecutiveMisses = 0;
        var readTask = messages.ReadAsync(cancellationToken).AsTask();
        var tickTask = ticks.WaitForNextTickAsync(cancellationToken).AsTask();

        while (true)
        {
            await Task.WhenAny(readTask, tickTask);
            cancellationToken.ThrowIfCancellationRequested();

            if (readTask.IsCompleted)
            {
                ProtectionChannelMessage message;
                try
                {
                    message = await readTask;
                }
                catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
                {
                    throw;
                }
                catch (EndOfStreamException)
                {
                    return ProtectionMonitorResult.Failed("SAFETY_IPC_CLOSED");
                }
                catch (IOException)
                {
                    return ProtectionMonitorResult.Failed("SAFETY_IPC_CLOSED");
                }
                catch (ChannelClosedException)
                {
                    return ProtectionMonitorResult.Failed("SAFETY_IPC_CLOSED");
                }
                catch (ProtectionPipeProtocolException exception)
                {
                    return ProtectionMonitorResult.Failed(exception.ErrorCode);
                }
                catch
                {
                    return ProtectionMonitorResult.Failed("SAFETY_IPC_MESSAGE_INVALID");
                }

                if (message.Kind == ProtectionMessageKind.Fatal)
                {
                    return ProtectionMonitorResult.Failed(message.FatalCode switch
                    {
                        ProtectionFatalCode.HookIntegrityFailed => "HOOK_INTEGRITY_FAILED",
                        ProtectionFatalCode.HeartbeatStopped => "HEARTBEAT_STOPPED",
                        ProtectionFatalCode.ProtectionThreadFailed => "PROTECTION_THREAD_FAILED",
                        _ => "SAFETY_IPC_PROTOCOL_INVALID"
                    });
                }
                if (message.Kind != ProtectionMessageKind.Heartbeat ||
                    message.DeniedCounters.Count != DeniedCounterCount)
                {
                    return ProtectionMonitorResult.Failed("SAFETY_IPC_PROTOCOL_INVALID");
                }
                if (lastSequence == ulong.MaxValue ||
                    message.Sequence != lastSequence + 1)
                {
                    return ProtectionMonitorResult.Failed("SAFETY_HEARTBEAT_SEQUENCE_INVALID");
                }
                if (message.MonotonicMilliseconds <= lastMonotonicMilliseconds)
                {
                    return ProtectionMonitorResult.Failed("SAFETY_HEARTBEAT_CLOCK_INVALID");
                }
                if (message.ActiveFlags != expectedActiveFlags)
                {
                    return ProtectionMonitorResult.Failed("SAFETY_FLAGS_CHANGED");
                }
                for (var index = 0; index < DeniedCounterCount; index++)
                {
                    if (message.DeniedCounters[index] < lastCounters[index])
                    {
                        return ProtectionMonitorResult.Failed("SAFETY_DENIED_COUNTER_ROLLBACK");
                    }
                    lastCounters[index] = message.DeniedCounters[index];
                }

                lastSequence = message.Sequence;
                lastMonotonicMilliseconds = message.MonotonicMilliseconds;
                receivedSinceTick = true;
                readTask = messages.ReadAsync(cancellationToken).AsTask();
            }

            if (tickTask.IsCompleted)
            {
                if (!await tickTask)
                {
                    return ProtectionMonitorResult.Failed("HEARTBEAT_STOPPED");
                }

                consecutiveMisses = receivedSinceTick ? 0 : consecutiveMisses + 1;
                receivedSinceTick = false;
                if (consecutiveMisses == MaximumConsecutiveMisses)
                {
                    return ProtectionMonitorResult.Failed("HEARTBEAT_TIMEOUT");
                }
                tickTask = ticks.WaitForNextTickAsync(cancellationToken).AsTask();
            }
        }
    }
}
