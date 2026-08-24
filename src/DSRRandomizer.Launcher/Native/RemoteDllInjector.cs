using System.Buffers.Binary;
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using DSRRandomizer.Launcher.Safety;

namespace DSRRandomizer.Launcher.Native;

[Flags]
public enum ProtectionFlags : ulong
{
    None = 0,
    Bootstrap = 1UL << 0
}

public sealed record GuardConfiguration(
    string GuardDllPath,
    ushort ProtocolVersion,
    ulong RequiredFlags,
    bool DiagnosticMode,
    byte[] ExpectedNonce,
    byte[] InitializationNonce,
    TimeSpan OperationTimeout,
    TimeSpan HandshakeTimeout)
{
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
            TimeSpan.FromSeconds(10));
    }
}

public sealed class RemoteDllInjector
{
    private const uint ProtectionMagic = 0x44535252;
    private const int InitBlockLength = 308;
    private const int PipeNameCharacters = 128;

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
        try
        {
            var pathBytes = Encoding.Unicode.GetBytes(canonicalGuardPath + '\0');
            remotePath = AllocateWriteAndVerify(remote.ProcessHandle, pathBytes);

            var loadLibraryAddress = ResolveBootstrapLoadLibrary();
            var loadStatus = await RunRemoteCallFailClosedAsync(
                child,
                remote.ProcessHandle,
                loadLibraryAddress,
                remotePath,
                configuration.OperationTimeout,
                cancellationToken,
                () => remoteMemoryCanBeReleased = false);
            if (loadStatus == 0)
            {
                return ProtectionHandshake.Failed("SAFETY_LOAD_LIBRARY_FAILED");
            }

            var remoteGuardBase = FindRemoteModuleBase(child.ProcessId, canonicalGuardPath);
            var initializerRva = PeExportReader.ReadExportRva(
                canonicalGuardPath,
                "InitializeProtection");
            var initializerAddress = AddAddress(remoteGuardBase, initializerRva);

            await using var pipe = new ProtectionPipeServer(
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
                return ProtectionHandshake.Failed("SAFETY_INITIALIZER_FAILED");
            }

            return await handshakeTask;
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
            if (remoteMemoryCanBeReleased)
            {
                FreeRemoteMemory(remote.ProcessHandle, remoteBlock);
                FreeRemoteMemory(remote.ProcessHandle, remotePath);
            }
        }
    }

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

    private static byte[] CreateInitBlock(
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
        pipeNameBytes.CopyTo(block, 52);
        return block;
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
        var snapshotPointer = NativeMethods.CreateToolhelp32Snapshot(
            NativeMethods.SnapshotModules | NativeMethods.SnapshotModules32,
            checked((uint)processId));
        if (snapshotPointer == IntPtr.Zero || snapshotPointer == new IntPtr(-1))
        {
            throw new SafetyLaunchException("SAFETY_MODULE_SNAPSHOT_FAILED");
        }

        using var snapshot = new SafeProcessHandle(snapshotPointer);
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
