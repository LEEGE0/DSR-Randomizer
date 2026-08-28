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
        IProtectionSession? session = null;
        using var stopMonitor = new CancellationTokenSource();
        var terminated = 0;
        void TerminateOnce()
        {
            if (process is not null && Interlocked.Exchange(ref terminated, 1) == 0)
            {
                process.TerminateJob();
            }
        }
        try
        {
            process = await _platform.CreateSuspendedAsync(request, cancellationToken);
            process.AssignKillOnCloseJob();

            var handshake = await process.InjectAndInitializeAsync(cancellationToken);
            session = handshake.Session;
            if (!handshake.Success)
            {
                TerminateOnce();
                return SafetyLaunchResult.Failed(handshake.ErrorCode);
            }

            if (handshake.ActiveFlags != request.RequiredProtectionFlags)
            {
                TerminateOnce();
                return SafetyLaunchResult.Failed("SAFETY_FLAGS_INCOMPLETE");
            }

            var simplifiedOneShot =
                SimplifiedOfflineProtection.IsExact(request.RequiredProtectionFlags) &&
                SimplifiedOfflineProtection.IsExact(handshake.ActiveFlags);
            if (simplifiedOneShot && session is not null)
            {
                TerminateOnce();
                return SafetyLaunchResult.Failed("SAFETY_MONITOR_UNEXPECTED");
            }
            if (!simplifiedOneShot && session is null)
            {
                TerminateOnce();
                return SafetyLaunchResult.Failed("SAFETY_MONITOR_UNAVAILABLE");
            }

            var previousSuspendCount = process.ResumeMainThread();
            if (previousSuspendCount != 1)
            {
                TerminateOnce();
                return SafetyLaunchResult.Failed("SAFETY_RESUME_COUNT_INVALID");
            }

            var exitTask = process.WaitForExitAsync(cancellationToken);
            if (simplifiedOneShot)
            {
                return new SafetyLaunchResult(
                    true,
                    string.Empty,
                    await exitTask);
            }

            var monitorTask = session!.MonitorAsync(
                request.RequiredProtectionFlags,
                stopMonitor.Token);
            _ = await Task.WhenAny(exitTask, monitorTask);
            if (exitTask.IsCompletedSuccessfully)
            {
                var exitCode = await exitTask;
                stopMonitor.Cancel();
                try
                {
                    _ = await monitorTask;
                }
                catch (OperationCanceledException) when (stopMonitor.IsCancellationRequested)
                {
                }
                return new SafetyLaunchResult(true, string.Empty, exitCode);
            }

            if (monitorTask.IsCompleted)
            {
                var monitorResult = await monitorTask;
                if (monitorResult.ErrorCode == "SAFETY_IPC_CLOSED")
                {
                    _ = await Task.WhenAny(
                        exitTask,
                        Task.Delay(TimeSpan.FromMilliseconds(100)));
                    if (exitTask.IsCompletedSuccessfully)
                    {
                        return new SafetyLaunchResult(
                            true,
                            string.Empty,
                            await exitTask);
                    }
                    if (exitTask.IsCanceled || exitTask.IsFaulted)
                    {
                        _ = await exitTask;
                    }
                }
                TerminateOnce();
                return SafetyLaunchResult.Failed(monitorResult.ErrorCode);
            }

            _ = await exitTask;
            throw new InvalidOperationException("The process wait completed without a result.");
        }
        catch (OperationCanceledException)
        {
            TerminateOnce();
            throw;
        }
        catch (SafetyLaunchException exception)
        {
            TerminateOnce();
            return SafetyLaunchResult.Failed(exception.ErrorCode);
        }
        catch
        {
            TerminateOnce();
            return SafetyLaunchResult.Failed("SAFETY_UNEXPECTED_FAILURE");
        }
        finally
        {
            stopMonitor.Cancel();
            try
            {
                if (session is not null)
                {
                    await session.DisposeAsync();
                }
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
}
