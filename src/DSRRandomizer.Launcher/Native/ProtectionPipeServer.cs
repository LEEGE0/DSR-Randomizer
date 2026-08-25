using System.Buffers.Binary;
using System.IO.Pipes;
using System.Security.AccessControl;
using System.Security.Cryptography;
using System.Security.Principal;
using DSRRandomizer.Launcher.Safety;

namespace DSRRandomizer.Launcher.Native;

internal sealed record ProtectionFrameParseResult(
    bool Success,
    string ErrorCode,
    ProtectionChannelMessage? Message)
{
    public static ProtectionFrameParseResult Invalid() =>
        new(false, "SAFETY_IPC_PROTOCOL_INVALID", null);
}

internal static class ProtectionPipeProtocol
{
    internal const uint ProtectionMagic = 0x44535252;
    internal const ushort ProtocolVersion = 2;
    internal const int NonceLength = 32;
    internal const int HeaderLength = 44;
    internal const int HandshakeLength = 56;
    internal const int FatalLength = 48;
    internal const int HeartbeatLength = 116;
    internal const int DeniedCounterCount = 6;

    public static ProtectionFrameParseResult ParseFrame(
        ReadOnlySpan<byte> frame,
        ReadOnlySpan<byte> expectedNonce)
    {
        if (expectedNonce.Length != NonceLength || frame.Length < HeaderLength)
        {
            return ProtectionFrameParseResult.Invalid();
        }

        var magic = BinaryPrimitives.ReadUInt32LittleEndian(frame);
        var version = BinaryPrimitives.ReadUInt16LittleEndian(frame[4..]);
        var size = BinaryPrimitives.ReadUInt16LittleEndian(frame[6..]);
        var rawKind = BinaryPrimitives.ReadUInt32LittleEndian(frame[40..]);
        if (magic != ProtectionMagic || version != ProtocolVersion ||
            size != frame.Length ||
            !Enum.IsDefined(typeof(ProtectionMessageKind), rawKind))
        {
            return ProtectionFrameParseResult.Invalid();
        }
        if (!CryptographicOperations.FixedTimeEquals(
                expectedNonce,
                frame.Slice(8, NonceLength)))
        {
            return new ProtectionFrameParseResult(
                false,
                "SAFETY_IPC_AUTH_FAILED",
                null);
        }

        var kind = (ProtectionMessageKind)rawKind;
        switch (kind)
        {
            case ProtectionMessageKind.Handshake when frame.Length == HandshakeLength:
                return new ProtectionFrameParseResult(
                    true,
                    string.Empty,
                    ProtectionChannelMessage.Handshake(
                        BinaryPrimitives.ReadUInt32LittleEndian(frame[44..]),
                        BinaryPrimitives.ReadUInt64LittleEndian(frame[48..])));

            case ProtectionMessageKind.Heartbeat when frame.Length == HeartbeatLength:
            {
                var counters = new ulong[DeniedCounterCount];
                for (var index = 0; index < counters.Length; index++)
                {
                    counters[index] = BinaryPrimitives.ReadUInt64LittleEndian(
                        frame[(68 + index * sizeof(ulong))..]);
                }
                return new ProtectionFrameParseResult(
                    true,
                    string.Empty,
                    ProtectionChannelMessage.Heartbeat(
                        BinaryPrimitives.ReadUInt64LittleEndian(frame[44..]),
                        BinaryPrimitives.ReadUInt64LittleEndian(frame[52..]),
                        BinaryPrimitives.ReadUInt64LittleEndian(frame[60..]),
                        counters));
            }

            case ProtectionMessageKind.Fatal when frame.Length == FatalLength:
            {
                var rawFatalCode = BinaryPrimitives.ReadUInt32LittleEndian(frame[44..]);
                if (!Enum.IsDefined(typeof(ProtectionFatalCode), rawFatalCode))
                {
                    return ProtectionFrameParseResult.Invalid();
                }
                return new ProtectionFrameParseResult(
                    true,
                    string.Empty,
                    ProtectionChannelMessage.Fatal((ProtectionFatalCode)rawFatalCode));
            }

            default:
                return ProtectionFrameParseResult.Invalid();
        }
    }

    public static bool IsDeclaredLengthSupported(ushort size) =>
        size is HandshakeLength or FatalLength or HeartbeatLength;
}

internal sealed class ProtectionPipeProtocolException(string errorCode) : Exception(errorCode)
{
    public string ErrorCode { get; } = errorCode;
}

public sealed class ProtectionPipeServer :
    IAsyncDisposable,
    IProtectionSession,
    IProtectionMessageSource
{
    private readonly byte[] _expectedNonce;
    private readonly TimeSpan _timeout;
    private readonly CancellationTokenSource _shutdown = new();
    private readonly NamedPipeServerStream _pipe;
    private int _disposed;
    private int _handshakeAccepted;
    private int _monitorStarted;

    public ProtectionPipeServer(byte[] expectedNonce, TimeSpan timeout)
    {
        ArgumentNullException.ThrowIfNull(expectedNonce);
        if (expectedNonce.Length != ProtectionPipeProtocol.NonceLength)
        {
            throw new ArgumentException("Protection nonce must contain 32 bytes.", nameof(expectedNonce));
        }

        if (timeout <= TimeSpan.Zero)
        {
            throw new ArgumentOutOfRangeException(nameof(timeout));
        }

        _expectedNonce = (byte[])expectedNonce.Clone();
        _timeout = timeout;
        PipeName = "DSRRandomizer-" + Convert.ToHexString(_expectedNonce).ToLowerInvariant();
        FullPipeName = @"\\.\pipe\" + PipeName;
        var security = CreatePipeSecurity();
        _pipe = NamedPipeServerStreamAcl.Create(
            PipeName,
            PipeDirection.In,
            maxNumberOfServerInstances: 1,
            PipeTransmissionMode.Byte,
            PipeOptions.Asynchronous,
            inBufferSize: ProtectionPipeProtocol.HeartbeatLength,
            outBufferSize: 0,
            security,
            HandleInheritability.None,
            additionalAccessRights: 0);
    }

    public string PipeName { get; }

    public string FullPipeName { get; }

    internal PipeSecurity GetAccessControlForTesting() => _pipe.GetAccessControl();

    public async Task<ProtectionHandshake> WaitForHandshakeAsync(
        CancellationToken cancellationToken)
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);

        using var timeout = new CancellationTokenSource(_timeout);
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken,
            timeout.Token,
            _shutdown.Token);
        try
        {
            await _pipe.WaitForConnectionAsync(linked.Token);
            var parsed = await ReadFrameAsync(linked.Token);
            if (!parsed.Success)
            {
                return ProtectionHandshake.Failed(parsed.ErrorCode);
            }
            if (parsed.Message?.Kind != ProtectionMessageKind.Handshake)
            {
                return ProtectionHandshake.Failed("SAFETY_IPC_PROTOCOL_INVALID");
            }

            if (parsed.Message.Status != 0)
            {
                return ProtectionHandshake.Failed("SAFETY_GUARD_REPORTED_FAILURE");
            }

            Volatile.Write(ref _handshakeAccepted, 1);
            return new ProtectionHandshake(
                true,
                parsed.Message.ActiveFlags,
                string.Empty);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (OperationCanceledException) when (timeout.IsCancellationRequested)
        {
            return ProtectionHandshake.Failed("SAFETY_IPC_TIMEOUT");
        }
        catch (OperationCanceledException) when (_shutdown.IsCancellationRequested)
        {
            return ProtectionHandshake.Failed("SAFETY_IPC_CLOSED");
        }
        catch (ProtectionPipeProtocolException exception)
        {
            return ProtectionHandshake.Failed(exception.ErrorCode);
        }
        catch (EndOfStreamException)
        {
            return ProtectionHandshake.Failed("SAFETY_IPC_MESSAGE_INVALID");
        }
        catch (IOException) when (_shutdown.IsCancellationRequested)
        {
            return ProtectionHandshake.Failed("SAFETY_IPC_CLOSED");
        }
        catch (IOException)
        {
            return ProtectionHandshake.Failed("SAFETY_IPC_CLOSED");
        }
    }

    public Task<ProtectionMonitorResult> MonitorAsync(
        ulong expectedActiveFlags,
        CancellationToken cancellationToken)
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
        if (Volatile.Read(ref _handshakeAccepted) == 0)
        {
            throw new InvalidOperationException("The protection handshake has not completed.");
        }
        if (Interlocked.Exchange(ref _monitorStarted, 1) != 0)
        {
            throw new InvalidOperationException("The protection session can only be monitored once.");
        }

        return new ProtectionHeartbeatMonitor(
            this,
            new PeriodicProtectionTickSource()).MonitorAsync(
                expectedActiveFlags,
                cancellationToken);
    }

    async ValueTask<ProtectionChannelMessage> IProtectionMessageSource.ReadAsync(
        CancellationToken cancellationToken)
    {
        var parsed = await ReadFrameAsync(cancellationToken);
        if (!parsed.Success || parsed.Message is null)
        {
            throw new ProtectionPipeProtocolException(parsed.ErrorCode);
        }
        return parsed.Message;
    }

    public ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref _disposed, 1) == 0)
        {
            _shutdown.Cancel();
            _pipe.Dispose();
            _shutdown.Dispose();
        }

        return ValueTask.CompletedTask;
    }

    private async Task<ProtectionFrameParseResult> ReadFrameAsync(
        CancellationToken cancellationToken)
    {
        var header = new byte[ProtectionPipeProtocol.HeaderLength];
        await _pipe.ReadExactlyAsync(header, cancellationToken);
        var size = BinaryPrimitives.ReadUInt16LittleEndian(header.AsSpan(6));
        if (!ProtectionPipeProtocol.IsDeclaredLengthSupported(size))
        {
            return ProtectionFrameParseResult.Invalid();
        }

        var frame = new byte[size];
        header.CopyTo(frame, 0);
        await _pipe.ReadExactlyAsync(
            frame.AsMemory(ProtectionPipeProtocol.HeaderLength),
            cancellationToken);
        return ProtectionPipeProtocol.ParseFrame(frame, _expectedNonce);
    }

    private static PipeSecurity CreatePipeSecurity()
    {
        using var identity = WindowsIdentity.GetCurrent(TokenAccessLevels.Query);
        var currentUser = identity.User ??
            throw new SafetyLaunchException("SAFETY_CURRENT_USER_SID_UNAVAILABLE");
        var localSystem = new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null);
        var security = new PipeSecurity();
        security.SetAccessRuleProtection(isProtected: true, preserveInheritance: false);
        security.AddAccessRule(new PipeAccessRule(
            currentUser,
            PipeAccessRights.FullControl,
            AccessControlType.Allow));
        security.AddAccessRule(new PipeAccessRule(
            localSystem,
            PipeAccessRights.FullControl,
            AccessControlType.Allow));
        return security;
    }
}
