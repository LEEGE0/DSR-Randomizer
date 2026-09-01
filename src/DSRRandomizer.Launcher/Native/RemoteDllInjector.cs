using System.Buffers.Binary;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using DSRRandomizer.Launcher.Safety;

namespace DSRRandomizer.Launcher.Native;

[Flags]
public enum ProtectionFlags : ulong
{
    None = 0,
    Bootstrap = 1UL << 0,
    SaveKnownFolder = 1UL << 1,
    SaveFileIo = 1UL << 2,
    Winsock = 1UL << 3,
    SteamInterfaces = 1UL << 4,
    DeferredModuleGate = 1UL << 5,
    GameServiceOffline = 1UL << 6,
    Heartbeat = 1UL << 7,
    HookIntegrity = 1UL << 8,
    SaveCallsiteRedirect = 1UL << 9
}

public sealed record GuardSavePathConfiguration(
    string VirtualDocuments,
    string VirtualLogicalSave,
    string RealSaveRoot,
    string ExternalSaveRoot,
    string DedicatedRmm);

public enum GuardSocketTransport : ushort
{
    Tcp = 1,
    Udp = 2
}

public sealed record GuardSocketEndpoint(
    GuardSocketTransport Transport,
    AddressFamily Family,
    ushort Port,
    IPAddress Address);

public sealed record GuardConfiguration(
    string GuardDllPath,
    ushort ProtocolVersion,
    ulong RequiredFlags,
    bool DiagnosticMode,
    byte[] ExpectedNonce,
    byte[] InitializationNonce,
    TimeSpan OperationTimeout,
    TimeSpan HandshakeTimeout,
    GuardSavePathConfiguration? SavePaths)
{
    public IReadOnlyList<GuardSocketEndpoint> SocketEndpoints { get; init; } =
        Array.Empty<GuardSocketEndpoint>();

    public static GuardConfiguration Create(
        string guardDllPath,
        ushort ProtocolVersion,
        ulong RequiredFlags,
        bool DiagnosticMode)
    {
        var nonce = RandomNumberGenerator.GetBytes(32);
        return new GuardConfiguration(
            guardDllPath,
            ProtocolVersion,
            RequiredFlags,
            DiagnosticMode,
            nonce,
            (byte[])nonce.Clone(),
            TimeSpan.FromSeconds(10),
            TimeSpan.FromSeconds(10),
            null);
    }
}

public sealed class RemoteDllInjector
{
    private const uint ProtectionMagic = 0x44535252;
    private const int InitBlockLength = 5480;
    private const int PipeNameCharacters = 128;
    private const int SavePathCharacters = 512;
    private const int PipeNameOffset = 52;
    private const int VirtualDocumentsOffset = 308;
    private const int VirtualLogicalSaveOffset = 1332;
    private const int RealSaveRootOffset = 2356;
    private const int ExternalSaveRootOffset = 3380;
    private const int DedicatedRmmOffset = 4404;
    private const int SocketEndpointCountOffset = 5428;
    private const int SocketEndpointsOffset = 5432;
    private const int SocketEndpointLength = 24;
    private const int SocketEndpointCapacity = 2;

    public async Task<ProtectionHandshake> InitializeAsync(
        IProtectedProcess child,
        GuardConfiguration configuration,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(child);
        ArgumentNullException.ThrowIfNull(configuration);
        ValidateConfiguration(configuration);

        if (child is not IRemoteProcessAccessor remote)
        {
            return ProtectionHandshake.Failed("SAFETY_REMOTE_PROCESS_UNAVAILABLE");
        }

        var canonicalGuardPath = Path.GetFullPath(configuration.GuardDllPath);
        IntPtr remotePath = IntPtr.Zero;
        IntPtr remoteBlock = IntPtr.Zero;
        var remoteMemoryCanBeReleased = true;
        ProtectionPipeServer? pipe = null;
        var pipeOwnershipTransferred = false;
        try
        {
            var pathBytes = Encoding.Unicode.GetBytes(canonicalGuardPath + '\0');
            remotePath = AllocateWriteAndVerify(remote.ProcessHandle, pathBytes);

            var loadLibraryAddress = ResolveBootstrapLoadLibrary();
            _ = await RunRemoteCallFailClosedAsync(
                child,
                remote.ProcessHandle,
                loadLibraryAddress,
                remotePath,
                configuration.OperationTimeout,
                cancellationToken,
                () => remoteMemoryCanBeReleased = false);
            var remoteGuardBase = FindRemoteModuleBase(child.ProcessId, canonicalGuardPath);
            var initializerRva = PeExportReader.ReadExportRva(
                canonicalGuardPath,
                "InitializeProtection");
            var initializerAddress = AddAddress(remoteGuardBase, initializerRva);

            pipe = new ProtectionPipeServer(
                configuration.ExpectedNonce,
                configuration.HandshakeTimeout);
            var initBlock = CreateInitBlock(configuration, pipe.FullPipeName);
            remoteBlock = AllocateWriteAndVerify(remote.ProcessHandle, initBlock);
            var handshakeTask = pipe.WaitForHandshakeAsync(cancellationToken);
            var initializeStatus = await RunRemoteCallFailClosedAsync(
                child,
                remote.ProcessHandle,
                initializerAddress,
                remoteBlock,
                configuration.OperationTimeout,
                cancellationToken,
                () => remoteMemoryCanBeReleased = false);
            if (initializeStatus != 0)
            {
                await pipe.DisposeAsync();
                _ = await handshakeTask;
                return ProtectionHandshake.Failed(
                    InitializerErrorCode(initializeStatus));
            }

            var handshake = await handshakeTask;
            if (!handshake.Success)
            {
                await pipe.DisposeAsync();
                return handshake;
            }

            if (DedicatedSaveProtection.IsExact(configuration.RequiredFlags)
                || SimplifiedOfflineProtection.IsExact(configuration.RequiredFlags))
            {
                var oneShot = await pipe.CompleteOneShotAsync(
                    handshake,
                    configuration.RequiredFlags);
                pipe = null;
                return oneShot;
            }

            const ProtectionFlags monitorFlags =
                ProtectionFlags.Heartbeat | ProtectionFlags.HookIntegrity;
            if (((ProtectionFlags)configuration.RequiredFlags & monitorFlags) != monitorFlags)
            {
                await pipe.DisposeAsync();
                pipe = null;
                return ProtectionHandshake.Failed("SAFETY_MONITOR_UNAVAILABLE");
            }

            pipeOwnershipTransferred = true;
            return handshake with { Session = pipe };
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (SafetyLaunchException exception)
        {
            return ProtectionHandshake.Failed(exception.ErrorCode);
        }
        catch
        {
            return ProtectionHandshake.Failed("SAFETY_INJECTION_FAILED");
        }
        finally
        {
            if (pipe is not null && !pipeOwnershipTransferred)
            {
                await pipe.DisposeAsync();
            }
            if (remoteMemoryCanBeReleased)
            {
                FreeRemoteMemory(remote.ProcessHandle, remoteBlock);
                FreeRemoteMemory(remote.ProcessHandle, remotePath);
            }
        }
    }

    internal static string InitializerErrorCode(uint status) => status switch
    {
        10 => "GAME_SERVICE_PROFILE_MISMATCH",
        11 => "GAME_SERVICE_HOOK_FAILED",
        12 => "PROTECTION_CLEANUP_FAILED",
        13 => "SAVE_CALLSITE_PROFILE_MISMATCH",
        _ => "SAFETY_INITIALIZER_FAILED"
    };

    private static void ValidateConfiguration(GuardConfiguration configuration)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(configuration.GuardDllPath);
        if (configuration.ExpectedNonce is not { Length: 32 } ||
            configuration.InitializationNonce is not { Length: 32 })
        {
            throw new ArgumentException("Protection nonces must contain 32 bytes.", nameof(configuration));
        }

        if (configuration.OperationTimeout <= TimeSpan.Zero ||
            configuration.HandshakeTimeout <= TimeSpan.Zero)
        {
            throw new ArgumentOutOfRangeException(nameof(configuration));
        }
    }

    internal static byte[] CreateInitBlock(
        GuardConfiguration configuration,
        string fullPipeName)
    {
        var pipeNameBytes = Encoding.Unicode.GetBytes(fullPipeName + '\0');
        if (pipeNameBytes.Length > PipeNameCharacters * sizeof(char))
        {
            throw new SafetyLaunchException("SAFETY_PIPE_NAME_TOO_LONG");
        }

        var block = new byte[InitBlockLength];
        BinaryPrimitives.WriteUInt32LittleEndian(block, ProtectionMagic);
        BinaryPrimitives.WriteUInt16LittleEndian(block.AsSpan(4), configuration.ProtocolVersion);
        BinaryPrimitives.WriteUInt16LittleEndian(block.AsSpan(6), InitBlockLength);
        BinaryPrimitives.WriteUInt64LittleEndian(block.AsSpan(8), configuration.RequiredFlags);
        BinaryPrimitives.WriteUInt32LittleEndian(
            block.AsSpan(16),
            configuration.DiagnosticMode ? 1U : 0U);
        configuration.InitializationNonce.CopyTo(block, 20);
        pipeNameBytes.CopyTo(block, PipeNameOffset);

        const ProtectionFlags saveMask =
            ProtectionFlags.SaveKnownFolder |
            ProtectionFlags.SaveFileIo |
            ProtectionFlags.SaveCallsiteRedirect;
        var requestedSaveFlags = (ProtectionFlags)configuration.RequiredFlags & saveMask;
        if ((requestedSaveFlags & ProtectionFlags.SaveFileIo) != 0
            && (requestedSaveFlags & ProtectionFlags.SaveKnownFolder) == 0)
        {
            throw new ArgumentException(
                "Save file-I/O protection requires Known Folder protection.",
                nameof(configuration));
        }
        if ((requestedSaveFlags & ProtectionFlags.SaveCallsiteRedirect) != 0)
        {
            if (configuration.SavePaths is null)
            {
                throw new ArgumentException(
                    "The dedicated rmm path is required for callsite redirection.",
                    nameof(configuration));
            }
            WriteCanonicalPath(
                block,
                DedicatedRmmOffset,
                configuration.SavePaths.DedicatedRmm,
                configuration);
        }
        if ((requestedSaveFlags & ProtectionFlags.SaveKnownFolder) != 0)
        {
            if (configuration.SavePaths is null)
            {
                throw new ArgumentException(
                    "Canonical save paths are required when save protections are requested.",
                    nameof(configuration));
            }

            WriteCanonicalPath(
                block,
                VirtualDocumentsOffset,
                configuration.SavePaths.VirtualDocuments,
                configuration);
        }
        if ((requestedSaveFlags & ProtectionFlags.SaveFileIo) != 0)
        {
            WriteCanonicalPath(
                block,
                VirtualLogicalSaveOffset,
                configuration.SavePaths!.VirtualLogicalSave,
                configuration);
            WriteCanonicalPath(
                block,
                RealSaveRootOffset,
                configuration.SavePaths.RealSaveRoot,
                configuration);
            WriteCanonicalPath(
                block,
                ExternalSaveRootOffset,
                configuration.SavePaths.ExternalSaveRoot,
                configuration);
            WriteCanonicalPath(
                block,
                DedicatedRmmOffset,
                configuration.SavePaths.DedicatedRmm,
                configuration);
        }
        if (requestedSaveFlags == ProtectionFlags.None
            && configuration.SavePaths is not null)
        {
            throw new ArgumentException(
                "Save paths cannot be marshalled unless save protection is requested.",
                nameof(configuration));
        }
        WriteSocketEndpoints(block, configuration);
        return block;
    }

    private static void WriteSocketEndpoints(
        byte[] block,
        GuardConfiguration configuration)
    {
        ArgumentNullException.ThrowIfNull(configuration.SocketEndpoints);
        if (configuration.SocketEndpoints.Count > SocketEndpointCapacity)
        {
            throw new ArgumentException(
                "At most two exact socket endpoints can be configured.",
                nameof(configuration));
        }
        var winsockRequested =
            ((ProtectionFlags)configuration.RequiredFlags & ProtectionFlags.Winsock) != 0;
        if (!winsockRequested && configuration.SocketEndpoints.Count != 0)
        {
            throw new ArgumentException(
                "Socket endpoints require the Winsock protection flag.",
                nameof(configuration));
        }
        if (configuration.SocketEndpoints
            .GroupBy(endpoint => endpoint.Transport)
            .Any(group => group.Count() != 1))
        {
            throw new ArgumentException(
                "Socket endpoint transports must be unique.",
                nameof(configuration));
        }

        BinaryPrimitives.WriteUInt32LittleEndian(
            block.AsSpan(SocketEndpointCountOffset),
            checked((uint)configuration.SocketEndpoints.Count));
        for (var index = 0; index < configuration.SocketEndpoints.Count; index++)
        {
            var endpoint = configuration.SocketEndpoints[index];
            ArgumentNullException.ThrowIfNull(endpoint);
            ArgumentNullException.ThrowIfNull(endpoint.Address);
            if (!Enum.IsDefined(endpoint.Transport) || endpoint.Port == 0 ||
                endpoint.Family is not (AddressFamily.InterNetwork or AddressFamily.InterNetworkV6) ||
                endpoint.Address.AddressFamily != endpoint.Family ||
                !IPAddress.IsLoopback(endpoint.Address))
            {
                throw new ArgumentException(
                    "Socket endpoints must be exact TCP/UDP loopback addresses with a nonzero port.",
                    nameof(configuration));
            }

            var offset = SocketEndpointsOffset + index * SocketEndpointLength;
            BinaryPrimitives.WriteUInt16LittleEndian(
                block.AsSpan(offset),
                (ushort)endpoint.Transport);
            BinaryPrimitives.WriteUInt16LittleEndian(
                block.AsSpan(offset + 2),
                (ushort)endpoint.Family);
            BinaryPrimitives.WriteUInt16BigEndian(
                block.AsSpan(offset + 4),
                endpoint.Port);
            endpoint.Address.GetAddressBytes().CopyTo(block, offset + 8);
        }
    }

    private static void WriteCanonicalPath(
        byte[] block,
        int offset,
        string path,
        GuardConfiguration configuration)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        var canonical = Path.GetFullPath(path);
        if (!string.Equals(canonical, path, StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith(@"\\", StringComparison.Ordinal) ||
            path.Contains('~'))
        {
            throw new ArgumentException(
                "Save hook paths must be unambiguous canonical DOS paths.",
                nameof(configuration));
        }

        var bytes = Encoding.Unicode.GetBytes(path + '\0');
        if (bytes.Length > SavePathCharacters * sizeof(char))
        {
            throw new ArgumentException(
                "A save hook path exceeds the protocol capacity.",
                nameof(configuration));
        }
        bytes.CopyTo(block, offset);
    }

    private static IntPtr AllocateWriteAndVerify(
        SafeProcessHandle process,
        byte[] bytes)
    {
        var size = checked((UIntPtr)(uint)bytes.Length);
        var address = NativeMethods.VirtualAllocEx(
            process,
            IntPtr.Zero,
            size,
            NativeMethods.MemoryCommit | NativeMethods.MemoryReserve,
            NativeMethods.PageReadWrite);
        if (address == IntPtr.Zero)
        {
            throw new SafetyLaunchException("SAFETY_REMOTE_ALLOC_FAILED");
        }

        if (!NativeMethods.WriteProcessMemory(process, address, bytes, size, out var bytesWritten) ||
            bytesWritten != size)
        {
            FreeRemoteMemory(process, address);
            throw new SafetyLaunchException("SAFETY_REMOTE_WRITE_FAILED");
        }

        var readBack = new byte[bytes.Length];
        if (!NativeMethods.ReadProcessMemory(process, address, readBack, size, out var bytesRead) ||
            bytesRead != size ||
            !CryptographicOperations.FixedTimeEquals(bytes, readBack))
        {
            FreeRemoteMemory(process, address);
            throw new SafetyLaunchException("SAFETY_REMOTE_VERIFY_FAILED");
        }

        return address;
    }

    private static async Task<uint> RunRemoteCallAsync(
        SafeProcessHandle process,
        IntPtr startAddress,
        IntPtr parameter,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var threadPointer = NativeMethods.CreateRemoteThread(
            process,
            IntPtr.Zero,
            UIntPtr.Zero,
            startAddress,
            parameter,
            0,
            out _);
        if (threadPointer == IntPtr.Zero)
        {
            throw new SafetyLaunchException("SAFETY_REMOTE_THREAD_FAILED");
        }

        using var thread = new SafeProcessHandle(threadPointer);
        var stopwatch = Stopwatch.StartNew();
        while (stopwatch.Elapsed < timeout)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var waitResult = NativeMethods.WaitForSingleObject(thread, 50);
            if (waitResult == NativeMethods.WaitObject0)
            {
                if (!NativeMethods.GetExitCodeThread(thread, out var exitCode))
                {
                    throw new SafetyLaunchException("SAFETY_REMOTE_RESULT_FAILED");
                }

                return exitCode;
            }

            if (waitResult == NativeMethods.WaitFailed)
            {
                throw new SafetyLaunchException("SAFETY_REMOTE_WAIT_FAILED");
            }

            if (waitResult != NativeMethods.WaitTimeout)
            {
                throw new SafetyLaunchException("SAFETY_REMOTE_WAIT_INVALID");
            }

            await Task.Yield();
        }

        throw new SafetyLaunchException("SAFETY_REMOTE_TIMEOUT");
    }

    private static async Task<uint> RunRemoteCallFailClosedAsync(
        IProtectedProcess child,
        SafeProcessHandle process,
        IntPtr startAddress,
        IntPtr parameter,
        TimeSpan timeout,
        CancellationToken cancellationToken,
        Action markRemoteMemoryUnsafeToRelease)
    {
        try
        {
            return await RunRemoteCallAsync(
                process,
                startAddress,
                parameter,
                timeout,
                cancellationToken);
        }
        catch
        {
            if (!await TerminateAndConfirmExitAsync(child))
            {
                markRemoteMemoryUnsafeToRelease();
            }

            throw;
        }
    }

    private static async Task<bool> TerminateAndConfirmExitAsync(IProtectedProcess child)
    {
        try
        {
            child.TerminateJob();
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
            _ = await child.WaitForExitAsync(timeout.Token);
            return true;
        }
        catch
        {
            return false;
        }
    }

    private static IntPtr ResolveBootstrapLoadLibrary()
    {
        var localKernel = NativeMethods.GetModuleHandleW("kernel32.dll");
        var localExport = NativeMethods.GetProcAddress(localKernel, "LoadLibraryW");
        if (localKernel == IntPtr.Zero || localExport == IntPtr.Zero)
        {
            throw new SafetyLaunchException("SAFETY_KERNEL_EXPORT_FAILED");
        }

        return localExport;
    }

    private static IntPtr FindRemoteModuleBase(
        int processId,
        string expectedPath)
    {
        using var snapshot = CreateModuleSnapshot(processId);
        var entry = new NativeMethods.ModuleEntry32
        {
            Size = checked((uint)System.Runtime.InteropServices.Marshal.SizeOf<NativeMethods.ModuleEntry32>()),
            ModuleName = string.Empty,
            ExecutablePath = string.Empty
        };
        if (!NativeMethods.Module32FirstW(snapshot, ref entry))
        {
            throw new SafetyLaunchException("SAFETY_MODULE_ENUMERATION_FAILED");
        }

        var canonicalExpected = Path.GetFullPath(expectedPath);
        do
        {
            var candidate = Path.GetFullPath(entry.ExecutablePath);
            if (string.Equals(candidate, canonicalExpected, StringComparison.OrdinalIgnoreCase))
            {
                return entry.ModuleBaseAddress;
            }

            entry.Size = checked((uint)System.Runtime.InteropServices.Marshal.SizeOf<NativeMethods.ModuleEntry32>());
        }
        while (NativeMethods.Module32NextW(snapshot, ref entry));

        throw new SafetyLaunchException("SAFETY_REMOTE_MODULE_NOT_FOUND");
    }

    private static SafeProcessHandle CreateModuleSnapshot(int processId)
    {
        const int errorBadLength = 24;
        const int maximumAttempts = 5;
        for (var attempt = 0; attempt < maximumAttempts; attempt++)
        {
            var snapshotPointer = NativeMethods.CreateToolhelp32Snapshot(
                NativeMethods.SnapshotModules | NativeMethods.SnapshotModules32,
                checked((uint)processId));
            if (snapshotPointer != IntPtr.Zero && snapshotPointer != new IntPtr(-1))
            {
                return new SafeProcessHandle(snapshotPointer);
            }

            if (System.Runtime.InteropServices.Marshal.GetLastWin32Error() != errorBadLength)
            {
                break;
            }

            Thread.Yield();
        }

        throw new SafetyLaunchException("SAFETY_MODULE_SNAPSHOT_FAILED");
    }

    private static IntPtr AddAddress(IntPtr baseAddress, ulong offset)
    {
        var address = checked((ulong)baseAddress.ToInt64() + offset);
        return new IntPtr(checked((long)address));
    }

    private static void FreeRemoteMemory(SafeProcessHandle process, IntPtr address)
    {
        if (address != IntPtr.Zero)
        {
            _ = NativeMethods.VirtualFreeEx(
                process,
                address,
                UIntPtr.Zero,
                NativeMethods.MemoryRelease);
        }
    }
}

internal interface IRemoteProcessAccessor
{
    SafeProcessHandle ProcessHandle { get; }
}
