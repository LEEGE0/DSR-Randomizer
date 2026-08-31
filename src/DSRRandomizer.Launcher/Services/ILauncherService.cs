using DSRRandomizer.Foundation.Installation;
using DSRRandomizer.Foundation.Runtime;
using DSRRandomizer.Foundation.Saves;
using DSRRandomizer.Launcher.Safety;

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

    Task<RuntimeReadinessResult> GetModdedLaunchReadinessAsync(
        CancellationToken cancellationToken);

    Task<IReadOnlyList<SaveProfileCandidate>> DiscoverSaveProfilesAsync(
        CancellationToken cancellationToken);

    Task<DedicatedSaveResult> PrepareDedicatedSaveAsync(
        string steamId,
        bool firstCopyConfirmed,
        CancellationToken cancellationToken);

    Task<SafetyLaunchResult> LaunchModdedAsync(
        string steamId,
        CancellationToken cancellationToken);

    Task<RandomizerToolLaunchResult> LaunchItemRandomizerAsync(
        CancellationToken cancellationToken);

    Task<RandomizerToolLaunchResult> LaunchEnemyRandomizerAsync(
        CancellationToken cancellationToken);
}

public sealed record RandomizerToolLaunchResult(bool Started, string ErrorCode)
{
    public static RandomizerToolLaunchResult Success() => new(true, string.Empty);

    public static RandomizerToolLaunchResult Failed(string errorCode) => new(false, errorCode);
}
