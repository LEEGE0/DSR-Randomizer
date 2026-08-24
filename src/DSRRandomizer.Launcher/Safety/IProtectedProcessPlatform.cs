namespace DSRRandomizer.Launcher.Safety;

public interface IProtectedProcessPlatform
{
    Task<IProtectedProcess> CreateSuspendedAsync(
        SafetyLaunchRequest request,
        CancellationToken cancellationToken);
}

public interface IProtectedProcess : IAsyncDisposable
{
    int ProcessId { get; }

    void AssignKillOnCloseJob();

    Task<ProtectionHandshake> InjectAndInitializeAsync(
        CancellationToken cancellationToken);

    uint ResumeMainThread();

    void TerminateJob();

    Task<int> WaitForExitAsync(CancellationToken cancellationToken);
}
