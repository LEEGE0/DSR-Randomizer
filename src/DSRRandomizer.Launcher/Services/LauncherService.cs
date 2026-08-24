using DSRRandomizer.Foundation.Installation;
using DSRRandomizer.Foundation.Paths;
using DSRRandomizer.Foundation.Runtime;
using DSRRandomizer.Foundation.Saves;

namespace DSRRandomizer.Launcher.Services;

public sealed class LauncherService : ILauncherService
{
    private readonly string _localDataRoot;
    private readonly IPathCanonicalizer _canonicalizer;
    private readonly ISaveProfileLocator _saveProfileLocator;
    private readonly IFileAccess _fileAccess;

    public LauncherService(string localDataRoot)
        : this(localDataRoot, new WindowsKnownFolderProvider())
    {
    }

    public LauncherService(
        string localDataRoot,
        IKnownFolderProvider knownFolderProvider)
        : this(localDataRoot, knownFolderProvider, new SystemFileAccess())
    {
    }

    public LauncherService(
        string localDataRoot,
        IKnownFolderProvider knownFolderProvider,
        IFileAccess fileAccess)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(localDataRoot);
        ArgumentNullException.ThrowIfNull(knownFolderProvider);
        ArgumentNullException.ThrowIfNull(fileAccess);
        _localDataRoot = Path.GetFullPath(localDataRoot);
        _canonicalizer = new WindowsPathCanonicalizer();
        _saveProfileLocator = new WindowsSaveProfileLocator(knownFolderProvider);
        _fileAccess = fileAccess;
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

    public Task<IReadOnlyList<SaveProfileCandidate>> DiscoverSaveProfilesAsync(
        CancellationToken cancellationToken) =>
        _saveProfileLocator.DiscoverAsync(cancellationToken);

    public async Task<DedicatedSaveResult> PrepareDedicatedSaveAsync(
        string steamId,
        bool firstCopyConfirmed,
        CancellationToken cancellationToken)
    {
        var components = await CreateSaveComponentsAsync(cancellationToken);
        string destination;
        try
        {
            destination = SavePaths.GetDedicatedSave(
                _localDataRoot,
                steamId,
                components.Boundary);
        }
        catch (ArgumentException exception)
        {
            return DedicatedSaveResult.Fail(SaveErrorCode.InvalidSteamId, exception.Message);
        }
        catch (UnauthorizedAccessException exception)
        {
            return DedicatedSaveResult.Fail(SaveErrorCode.PathDenied, exception.Message);
        }

        var dedicatedSaveService = new DedicatedSaveService(
            components.Layout,
            components.Boundary,
            components.SelectionStore,
            firstCopyConfirmed
                ? _fileAccess
                : new NormalSaveBlockingFileAccess(_fileAccess));
        if (File.Exists(destination))
        {
            return await dedicatedSaveService.PrepareAsync(steamId, cancellationToken);
        }

        if (!firstCopyConfirmed)
        {
            return DedicatedSaveResult.Fail(
                SaveErrorCode.MultipleProfilesRequireSelection,
                "First-copy confirmation is required before reading the normal DRAKS0005.sl2 save.");
        }

        var candidates = await _saveProfileLocator.DiscoverAsync(cancellationToken);
        var selection = candidates.SingleOrDefault(candidate =>
            candidate.SteamId.Equals(steamId, StringComparison.Ordinal));
        if (selection is null)
        {
            return DedicatedSaveResult.Fail(
                SaveErrorCode.SourceMissing,
                "The selected SteamID does not have a discovered normal save profile.");
        }

        await components.SelectionStore.WriteAsync(selection, cancellationToken);
        return await dedicatedSaveService.PrepareAsync(steamId, cancellationToken);
    }

    private async Task<SaveComponents> CreateSaveComponentsAsync(
        CancellationToken cancellationToken)
    {
        var selectedInstallation = await InstallationSelectionStore
            .CreateReadOnly(_localDataRoot, _canonicalizer)
            .ReadAsync(cancellationToken);
        var protectedSource = selectedInstallation ?? Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
            "DSR-Randomizer-Protected-Source");
        var boundary = WriteBoundary.Create(
            protectedSource,
            _localDataRoot,
            _canonicalizer);
        var layout = LocalDataLayout.Create(_localDataRoot, boundary);
        return new SaveComponents(
            boundary,
            layout,
            new SaveSelectionStore(layout, boundary));
    }

    private sealed record SaveComponents(
        WriteBoundary Boundary,
        LocalDataLayout Layout,
        SaveSelectionStore SelectionStore);

    private sealed class NormalSaveBlockingFileAccess(IFileAccess inner) : IFileAccess
    {
        public bool Exists(string path) => IsNormalSave(path) ? false : inner.Exists(path);

        public IFileMutationLease AcquireMutationLease(
            string rootPath,
            IReadOnlyCollection<string> directoryPaths) =>
            inner.AcquireMutationLease(rootPath, directoryPaths);

        public FileAttributes GetAttributes(string path) => inner.GetAttributes(path);

        public Stream Open(string path, FileMode mode, FileAccess access, FileShare share)
        {
            if (IsNormalSave(path))
            {
                throw new UnauthorizedAccessException(
                    "The normal DRAKS0005.sl2 save cannot be opened before first-copy confirmation.");
            }

            return inner.Open(path, mode, access, share);
        }

        public Task<FileIdentityAndHash> IdentityAndHashAsync(
            Stream stream,
            CancellationToken cancellationToken) =>
            inner.IdentityAndHashAsync(stream, cancellationToken);

        public Task<FileIdentityAndHash> IdentityAndHashAsync(
            string path,
            CancellationToken cancellationToken) =>
            inner.IdentityAndHashAsync(path, cancellationToken);

        public Task CopyAndFlushAsync(
            Stream source,
            string destinationPath,
            CancellationToken cancellationToken) =>
            inner.CopyAndFlushAsync(source, destinationPath, cancellationToken);

        public Task<CreatedFileIdentity> WriteAllBytesAndFlushAsync(
            string path,
            ReadOnlyMemory<byte> bytes,
            CancellationToken cancellationToken) =>
            inner.WriteAllBytesAndFlushAsync(path, bytes, cancellationToken);

        public bool MoveCreateNewIfIdentityMatches(
            string sourcePath,
            string destinationPath,
            string expectedSourceIdentity) =>
            inner.MoveCreateNewIfIdentityMatches(
                sourcePath,
                destinationPath,
                expectedSourceIdentity);

        public bool ReplaceIfSourceIdentityMatches(
            string sourcePath,
            string destinationPath,
            string expectedSourceIdentity) =>
            inner.ReplaceIfSourceIdentityMatches(
                sourcePath,
                destinationPath,
                expectedSourceIdentity);

        public bool DeleteIfIdentityMatches(string path, string expectedIdentity) =>
            inner.DeleteIfIdentityMatches(path, expectedIdentity);

        private static bool IsNormalSave(string path) =>
            Path.GetFileName(path).Equals("DRAKS0005.sl2", StringComparison.OrdinalIgnoreCase);
    }
}
