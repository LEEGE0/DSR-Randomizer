using System.Buffers.Binary;
using System.IO.Pipes;
using System.Security.Cryptography;
using System.Security.AccessControl;
using System.Security.Principal;
using DSRRandomizer.Launcher.Safety;

namespace DSRRandomizer.Launcher.Native;

public sealed class ProtectionPipeServer : IAsyncDisposable
{
    private const uint ProtectionMagic = 0x44535252;
    private const ushort ProtocolVersion = 2;
    private const int NonceLength = 32;
    private const int MessageLength = 52;

    private readonly byte[] _expectedNonce;
    private readonly TimeSpan _timeout;
    private readonly CancellationTokenSource _shutdown = new();
    private readonly NamedPipeServerStream _pipe;
    private int _disposed;

    public ProtectionPipeServer(byte[] expectedNonce, TimeSpan timeout)
    {
        ArgumentNullException.ThrowIfNull(expectedNonce);
        if (expectedNonce.Length != NonceLength)
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
            inBufferSize: MessageLength,
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
            var message = new byte[MessageLength];
            await _pipe.ReadExactlyAsync(message, linked.Token);
            return ParseMessage(message);
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
        catch (EndOfStreamException)
        {
            return ProtectionHandshake.Failed("SAFETY_IPC_MESSAGE_INVALID");
        }
        catch (IOException) when (_shutdown.IsCancellationRequested)
        {
            return ProtectionHandshake.Failed("SAFETY_IPC_CLOSED");
        }
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

    private ProtectionHandshake ParseMessage(ReadOnlySpan<byte> message)
    {
        var magic = BinaryPrimitives.ReadUInt32LittleEndian(message);
        var version = BinaryPrimitives.ReadUInt16LittleEndian(message[4..]);
        var size = BinaryPrimitives.ReadUInt16LittleEndian(message[6..]);
        if (magic != ProtectionMagic || version != ProtocolVersion || size != MessageLength)
        {
            return ProtectionHandshake.Failed("SAFETY_IPC_PROTOCOL_INVALID");
        }

        if (!CryptographicOperations.FixedTimeEquals(
                _expectedNonce,
                message.Slice(8, NonceLength)))
        {
            return ProtectionHandshake.Failed("SAFETY_IPC_AUTH_FAILED");
        }

        var status = BinaryPrimitives.ReadUInt32LittleEndian(message[40..]);
        var activeFlags = BinaryPrimitives.ReadUInt64LittleEndian(message[44..]);
        return status == 0
            ? new ProtectionHandshake(true, activeFlags, string.Empty)
            : ProtectionHandshake.Failed("SAFETY_GUARD_REPORTED_FAILURE");
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
