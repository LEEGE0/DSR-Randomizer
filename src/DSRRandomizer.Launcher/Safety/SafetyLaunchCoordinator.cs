namespace DSRRandomizer.Launcher.Safety;

public sealed class SafetyLaunchCoordinator
{
    private readonly IProtectedProcessPlatform _platform;

    public SafetyLaunchCoordinator(IProtectedProcessPlatform platform)
    {
        _platform = platform ?? throw new ArgumentNullException(nameof(platform));
    }

    public async Task<SafetyLaunchResult> LaunchAsync(
        SafetyLaunchRequest request,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(request);

        IProtectedProcess? process = null;
        try
        {
            process = await _platform.CreateSuspendedAsync(request, cancellationToken);
            process.AssignKillOnCloseJob();

            var handshake = await process.InjectAndInitializeAsync(cancellationToken);
            if (!handshake.Success)
            {
                process.TerminateJob();
                return SafetyLaunchResult.Failed(handshake.ErrorCode);
            }

            if (handshake.ActiveFlags != request.RequiredProtectionFlags)
            {
                process.TerminateJob();
                return SafetyLaunchResult.Failed("SAFETY_FLAGS_INCOMPLETE");
            }

            var previousSuspendCount = process.ResumeMainThread();
            if (previousSuspendCount != 1)
            {
                process.TerminateJob();
                return SafetyLaunchResult.Failed("SAFETY_RESUME_COUNT_INVALID");
            }

            var exitCode = await process.WaitForExitAsync(cancellationToken);
            return new SafetyLaunchResult(true, string.Empty, exitCode);
        }
        catch (OperationCanceledException)
        {
            process?.TerminateJob();
            throw;
        }
        catch (SafetyLaunchException exception)
        {
            process?.TerminateJob();
            return SafetyLaunchResult.Failed(exception.ErrorCode);
        }
        catch
        {
            process?.TerminateJob();
            return SafetyLaunchResult.Failed("SAFETY_UNEXPECTED_FAILURE");
        }
        finally
        {
            if (process is not null)
            {
                await process.DisposeAsync();
            }
        }
    }
}
