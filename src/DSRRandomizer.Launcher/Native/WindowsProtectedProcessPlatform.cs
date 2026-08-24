using System.Runtime.InteropServices;
using System.Text;
using DSRRandomizer.Launcher.Safety;

namespace DSRRandomizer.Launcher.Native;

public sealed class WindowsProtectedProcessPlatform : IProtectedProcessPlatform
{
    public Task<IProtectedProcess> CreateSuspendedAsync(
        SafetyLaunchRequest request,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(request);
        cancellationToken.ThrowIfCancellationRequested();

        if (!OperatingSystem.IsWindows() || !Environment.Is64BitProcess)
        {
            throw new SafetyLaunchException("SAFETY_WINDOWS_X64_REQUIRED");
        }

        var job = CreateKillOnCloseJob();
        var environment = IntPtr.Zero;
        try
        {
            var startupInfo = new NativeMethods.StartupInfo
            {
                Size = checked((uint)Marshal.SizeOf<NativeMethods.StartupInfo>())
            };
            var commandLine = new StringBuilder(QuoteCommandLineArgument(request.ExecutablePath));
            environment = Marshal.StringToHGlobalUni(CreateMinimalEnvironmentBlock());
            if (!NativeMethods.CreateProcessW(
                    request.ExecutablePath,
                    commandLine,
                    IntPtr.Zero,
                    IntPtr.Zero,
                    inheritHandles: false,
                    NativeMethods.CreateSuspended | NativeMethods.CreateUnicodeEnvironment,
                    environment,
                    request.WorkingDirectory,
                    ref startupInfo,
                    out var processInformation))
            {
                throw new SafetyLaunchException("SAFETY_CREATE_FAILED");
            }

            var process = new SafeProcessHandle(processInformation.Process);
            var thread = new SafeProcessHandle(processInformation.Thread);
            return Task.FromResult<IProtectedProcess>(new WindowsProtectedProcess(
                checked((int)processInformation.ProcessId),
                process,
                thread,
                job,
                request));
        }
        catch
        {
            job.Dispose();
            throw;
        }
        finally
        {
            if (environment != IntPtr.Zero)
            {
                Marshal.FreeHGlobal(environment);
            }
        }
    }

    private static SafeJobHandle CreateKillOnCloseJob()
    {
        var job = NativeMethods.CreateJobObjectW(IntPtr.Zero, null);
        if (job.IsInvalid)
        {
            job.Dispose();
            throw new SafetyLaunchException("SAFETY_JOB_CREATE_FAILED");
        }

        var information = new NativeMethods.JobObjectExtendedLimitInformation();
        information.BasicLimitInformation.LimitFlags =
            NativeMethods.JobObjectLimitKillOnJobClose;
        if (!NativeMethods.SetInformationJobObject(
                job,
                NativeMethods.JobObjectInfoClass.ExtendedLimitInformation,
                ref information,
                checked((uint)Marshal.SizeOf<NativeMethods.JobObjectExtendedLimitInformation>())))
        {
            job.Dispose();
            throw new SafetyLaunchException("SAFETY_JOB_CONFIGURE_FAILED");
        }

        return job;
    }

    private static string CreateMinimalEnvironmentBlock()
    {
        var allowedNames = new[]
        {
            "APPDATA",
            "CommonProgramFiles",
            "CommonProgramFiles(x86)",
            "LOCALAPPDATA",
            "ProgramData",
            "ProgramFiles",
            "ProgramFiles(x86)",
            "SystemDrive",
            "SystemRoot",
            "TEMP",
            "TMP",
            "USERPROFILE",
            "WINDIR"
        };
        var entries = allowedNames
            .Select(name => (Name: name, Value: Environment.GetEnvironmentVariable(name)))
            .Where(entry => !string.IsNullOrEmpty(entry.Value))
            .OrderBy(entry => entry.Name, StringComparer.OrdinalIgnoreCase)
            .Select(entry => $"{entry.Name}={entry.Value}");
        return string.Join('\0', entries) + "\0";
    }

    internal static string QuoteCommandLineArgument(string value)
    {
        ArgumentException.ThrowIfNullOrEmpty(value);

        var quoted = new StringBuilder(value.Length + 2);
        quoted.Append('"');
        var backslashCount = 0;
        foreach (var character in value)
        {
            if (character == '\\')
            {
                backslashCount++;
                continue;
            }

            if (character == '"')
            {
                quoted.Append('\\', (backslashCount * 2) + 1);
                quoted.Append('"');
                backslashCount = 0;
                continue;
            }

            quoted.Append('\\', backslashCount);
            backslashCount = 0;
            quoted.Append(character);
        }

        quoted.Append('\\', backslashCount * 2);
        quoted.Append('"');
        return quoted.ToString();
    }

    private sealed class WindowsProtectedProcess(
        int processId,
        SafeProcessHandle process,
        SafeProcessHandle primaryThread,
        SafeJobHandle job,
        SafetyLaunchRequest request) : IProtectedProcess, IRemoteProcessAccessor
    {
        private int _disposed;

        public int ProcessId { get; } = processId;

        SafeProcessHandle IRemoteProcessAccessor.ProcessHandle => process;

        public void AssignKillOnCloseJob()
        {
            ThrowIfDisposed();
            if (!NativeMethods.AssignProcessToJobObject(job, process))
            {
                throw new SafetyLaunchException("SAFETY_JOB_ASSIGN_FAILED");
            }
        }

        public Task<ProtectionHandshake> InjectAndInitializeAsync(
            CancellationToken cancellationToken) =>
            new RemoteDllInjector().InitializeAsync(
                this,
                GuardConfiguration.Create(
                    request.GuardDllPath,
                    request.Profile.ProtocolVersion,
                    request.RequiredProtectionFlags,
                    request.DiagnosticMode),
                cancellationToken);

        public uint ResumeMainThread()
        {
            ThrowIfDisposed();
            var previousSuspendCount = NativeMethods.ResumeThread(primaryThread);
            if (previousSuspendCount == uint.MaxValue)
            {
                throw new SafetyLaunchException("SAFETY_RESUME_FAILED");
            }

            return previousSuspendCount;
        }

        public void TerminateJob()
        {
            if (Volatile.Read(ref _disposed) != 0)
            {
                return;
            }

            _ = NativeMethods.TerminateJobObject(job, 1);
            _ = NativeMethods.TerminateProcess(process, 1);
        }

        public async Task<int> WaitForExitAsync(CancellationToken cancellationToken)
        {
            ThrowIfDisposed();
            while (true)
            {
                cancellationToken.ThrowIfCancellationRequested();
                var waitResult = NativeMethods.WaitForSingleObject(process, 50);
                if (waitResult == NativeMethods.WaitObject0)
                {
                    if (!NativeMethods.GetExitCodeProcess(process, out var exitCode))
                    {
                        throw new SafetyLaunchException("SAFETY_EXIT_CODE_FAILED");
                    }

                    return unchecked((int)exitCode);
                }

                if (waitResult == NativeMethods.WaitFailed)
                {
                    throw new SafetyLaunchException("SAFETY_PROCESS_WAIT_FAILED");
                }

                if (waitResult != NativeMethods.WaitTimeout)
                {
                    throw new SafetyLaunchException("SAFETY_PROCESS_WAIT_INVALID");
                }

                await Task.Yield();
            }
        }

        public ValueTask DisposeAsync()
        {
            if (Interlocked.Exchange(ref _disposed, 1) != 0)
            {
                return ValueTask.CompletedTask;
            }

            job.Dispose();
            _ = NativeMethods.TerminateProcess(process, 1);
            primaryThread.Dispose();
            process.Dispose();
            return ValueTask.CompletedTask;
        }

        private void ThrowIfDisposed() =>
            ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
    }
}
