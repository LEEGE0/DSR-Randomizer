using DSRRandomizer.Foundation.Installation;
using DSRRandomizer.Foundation.Paths;
using DSRRandomizer.Foundation.Runtime;

namespace DSRRandomizer.Launcher.Services;

public sealed class LauncherService : ILauncherService
{
    private readonly string _localDataRoot;
    private readonly IPathCanonicalizer _canonicalizer;

    public LauncherService(string localDataRoot)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(localDataRoot);
        _localDataRoot = Path.GetFullPath(localDataRoot);
        _canonicalizer = new WindowsPathCanonicalizer();
    }

    public static LauncherService CreateDefault()
    {
        var localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        if (string.IsNullOrWhiteSpace(localAppData))
        {
            throw new InvalidOperationException("The Windows local application-data path is unavailable.");
        }

        return new LauncherService(Path.Combine(localAppData, "DSR-Randomizer"));
    }

    public Task<VerificationResult> VerifyAsync(
        string gamePath,
        CancellationToken cancellationToken) =>
        new GameInstallationVerifier(_canonicalizer, _localDataRoot)
            .VerifyAsync(gamePath, cancellationToken);

    public async Task<RuntimeManifest> InitializeRuntimeAsync(
        string gamePath,
        IProgress<RuntimeBuildProgress>? progress,
        CancellationToken cancellationToken)
    {
        var verification = await VerifyAsync(gamePath, cancellationToken);
        if (!verification.IsValid || verification.Catalog is null)
        {
            throw new InvalidOperationException(
                $"Installation verification failed: {string.Join("; ", verification.Errors)}");
        }

        var boundary = WriteBoundary.Create(
            verification.CanonicalInstallationPath,
            _localDataRoot,
            _canonicalizer);
        var layout = LocalDataLayout.Create(_localDataRoot, boundary);
        var pointerStore = new RuntimePointerStore(layout, boundary);
        var hashes = new FileHashService();
        var builder = new RuntimeBuilder(
            layout,
            boundary,
            new SystemFileCopier(),
            new DriveDiskSpaceProbe(),
            new SystemClock(),
            hashes,
            pointerStore);
        var manifest = await builder.BuildAsync(
            verification.CanonicalInstallationPath,
            verification.Catalog,
            progress,
            cancellationToken);
        await new InstallationSelectionStore(layout, boundary, _canonicalizer)
            .SaveAsync(verification.CanonicalInstallationPath, cancellationToken);
        return manifest;
    }

    public async Task<RuntimeReadinessResult> GetReadinessAsync(
        CancellationToken cancellationToken)
    {
        var selectedInstallation = await InstallationSelectionStore
            .CreateReadOnly(_localDataRoot, _canonicalizer)
            .ReadAsync(cancellationToken);
        if (selectedInstallation is null)
        {
            return new RuntimeReadinessResult(
                false,
                null,
                new[] { "The verified source-installation selection does not exist." });
        }

        var boundary = WriteBoundary.Create(
            selectedInstallation,
            _localDataRoot,
            _canonicalizer);
        var layout = LocalDataLayout.Create(_localDataRoot, boundary);
        var pointerStore = new RuntimePointerStore(layout, boundary);
        return await new RuntimeReadinessService(
                layout,
                boundary,
                _canonicalizer,
                new FileHashService(),
                pointerStore)
            .ValidateAsync(cancellationToken);
    }
}
