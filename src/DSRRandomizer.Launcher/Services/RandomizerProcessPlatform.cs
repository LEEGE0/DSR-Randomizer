using System.Runtime.InteropServices;
using System.Text;
using DSRRandomizer.Launcher.Native;
using DSRRandomizer.Launcher.Safety;

namespace DSRRandomizer.Launcher.Services;

internal sealed record RandomizerProcessRequest(
    string FileName,
    string WorkingDirectory,
    IReadOnlyList<string> Arguments);

internal interface IRandomizerProcessPlatform
{
    RandomizerToolLaunchResult StartTool(RandomizerProcessRequest request);

    Task<SafetyLaunchResult> LaunchModEngineAsync(
        RandomizerProcessRequest request,
        CancellationToken cancellationToken);
}

internal sealed class WindowsRandomizerProcessPlatform : IRandomizerProcessPlatform
{
    public RandomizerToolLaunchResult StartTool(RandomizerProcessRequest request)
    {
        using var process = Start(request);
        return RandomizerToolLaunchResult.Success();
    }

    public async Task<SafetyLaunchResult> LaunchModEngineAsync(
        RandomizerProcessRequest request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        using var process = Start(request);
        while (true)
        {
            var waitResult = NativeMethods.WaitForSingleObject(process, 50);
            if (waitResult == NativeMethods.WaitObject0)
            {
                if (!NativeMethods.GetExitCodeProcess(process, out var exitCode))
                {
                    throw new IOException("Unable to read the Mod Engine launcher exit code.");
                }
                var signedExitCode = unchecked((int)exitCode);
                return signedExitCode == 0
                    ? new SafetyLaunchResult(true, string.Empty, ExitCode: null)
                    : new SafetyLaunchResult(
                        false,
                        "MOD_ENGINE_LAUNCHER_FAILED",
                        signedExitCode);
            }
            if (waitResult == NativeMethods.WaitFailed)
            {
                throw new IOException("Waiting for the Mod Engine launcher failed.");
            }
            await Task.Delay(50, CancellationToken.None);
        }
    }

    private static SafeProcessHandle Start(RandomizerProcessRequest request)
    {
        ArgumentNullException.ThrowIfNull(request);
        var startupInfo = new NativeMethods.StartupInfo
        {
            Size = checked((uint)Marshal.SizeOf<NativeMethods.StartupInfo>())
        };
        var commandLine = new StringBuilder(
            WindowsProtectedProcessPlatform.QuoteCommandLineArgument(request.FileName));
        foreach (var argument in request.Arguments)
        {
            commandLine.Append(' ');
            commandLine.Append(
                WindowsProtectedProcessPlatform.QuoteCommandLineArgument(argument));
        }
        if (!NativeMethods.CreateProcessW(
                request.FileName,
                commandLine,
                IntPtr.Zero,
                IntPtr.Zero,
                inheritHandles: false,
                creationFlags: 0,
                IntPtr.Zero,
                request.WorkingDirectory,
                ref startupInfo,
                out var processInformation))
        {
            throw new IOException(
                $"Unable to start process; Windows error {Marshal.GetLastWin32Error()}.");
        }

        NativeMethods.CloseHandle(processInformation.Thread);
        return new SafeProcessHandle(processInformation.Process);
    }
}
