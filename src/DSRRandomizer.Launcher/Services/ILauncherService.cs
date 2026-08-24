using DSRRandomizer.Foundation.Installation;
using DSRRandomizer.Foundation.Runtime;

namespace DSRRandomizer.Launcher.Services;

public interface ILauncherService
{
    Task<VerificationResult> VerifyAsync(
        string gamePath,
        CancellationToken cancellationToken);

    Task<RuntimeManifest> InitializeRuntimeAsync(
        string gamePath,
        IProgress<RuntimeBuildProgress>? progress,
        CancellationToken cancellationToken);

    Task<RuntimeReadinessResult> GetReadinessAsync(
        CancellationToken cancellationToken);
}
