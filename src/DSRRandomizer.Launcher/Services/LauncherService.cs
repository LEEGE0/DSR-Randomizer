using System.Text;
using System.Text.Json;
using DSRRandomizer.Foundation.Installation;
using DSRRandomizer.Foundation.Paths;
using DSRRandomizer.Foundation.Runtime;
using DSRRandomizer.Foundation.Safety;
using DSRRandomizer.Foundation.Saves;
using DSRRandomizer.Launcher.Native;
using DSRRandomizer.Launcher.Safety;

namespace DSRRandomizer.Launcher.Services;

public sealed class LauncherService : ILauncherService
{
    private readonly string _externalRoot;
    private readonly IPathCanonicalizer _canonicalizer;
    private readonly ISaveProfileLocator _saveProfileLocator;
    private readonly IFileAccess _fileAccess;
    private readonly IKnownFolderProvider _knownFolderProvider;
    private readonly CompatibilityProfileCatalog? _profiles;
    private readonly IProtectedProcessPlatform _platform;
    private readonly string _guardDllPath;
    private readonly string _profilePath;
    private readonly LaunchArtifactIdentities _artifactIdentities;

    public LauncherService(string externalRoot)
        : this(externalRoot, new WindowsKnownFolderProvider())
    {
    }

    public LauncherService(
        string externalRoot,
        IKnownFolderProvider knownFolderProvider)
        : this(externalRoot, knownFolderProvider, new SystemFileAccess())
    {
    }

    public LauncherService(
        string externalRoot,
        IKnownFolderProvider knownFolderProvider,
        IFileAccess fileAccess)
        : this(
            externalRoot,
            knownFolderProvider,
            fileAccess,
            profiles: null,
            new WindowsProtectedProcessPlatform(),
            Path.Combine(AppContext.BaseDirectory, "native", "DSRRandomizer.Runtime.dll"),
            Path.Combine(AppContext.BaseDirectory, "config", "compatibility-profiles.json"),
            LaunchArtifactIdentities.LoadEmbedded())
    {
    }

    internal LauncherService(
        string externalRoot,
        IKnownFolderProvider knownFolderProvider,
        IFileAccess fileAccess,
        CompatibilityProfileCatalog? profiles,
        IProtectedProcessPlatform platform,
        string guardDllPath,
        string profilePath,
        LaunchArtifactIdentities artifactIdentities)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(externalRoot);
        ArgumentNullException.ThrowIfNull(knownFolderProvider);
        ArgumentNullException.ThrowIfNull(fileAccess);
        ArgumentNullException.ThrowIfNull(platform);
        ArgumentException.ThrowIfNullOrWhiteSpace(guardDllPath);
        ArgumentException.ThrowIfNullOrWhiteSpace(profilePath);
        ArgumentNullException.ThrowIfNull(artifactIdentities);
        _externalRoot = Path.GetFullPath(externalRoot);
        _canonicalizer = new WindowsPathCanonicalizer();
        _saveProfileLocator = new WindowsSaveProfileLocator(knownFolderProvider);
        _fileAccess = fileAccess;
        _knownFolderProvider = knownFolderProvider;
        _profiles = profiles;
        _platform = platform;
        _guardDllPath = Path.GetFullPath(guardDllPath);
        _profilePath = Path.GetFullPath(profilePath);
        _artifactIdentities = artifactIdentities;
    }

    public Task<VerificationResult> VerifyAsync(
        string gamePath,
        CancellationToken cancellationToken) =>
        new GameInstallationVerifier(_canonicalizer, _externalRoot)
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
            _externalRoot,
            _canonicalizer);
        var layout = LocalDataLayout.Create(_externalRoot, boundary);
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
            .CreateReadOnly(_externalRoot, _canonicalizer)
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
            _externalRoot,
            _canonicalizer);
        var layout = LocalDataLayout.Create(_externalRoot, boundary);
        var pointerStore = new RuntimePointerStore(layout, boundary);
        return await new RuntimeReadinessService(
                layout,
                boundary,
                _canonicalizer,
                new FileHashService(),
                pointerStore)
            .ValidateAsync(cancellationToken);
    }

    public Task<RuntimeReadinessResult> GetModdedLaunchReadinessAsync(
        CancellationToken cancellationToken) => Task.Run(
        async () =>
        {
            var selectedInstallation = await InstallationSelectionStore
                .CreateReadOnly(_externalRoot, _canonicalizer)
                .ReadAsync(cancellationToken)
                .ConfigureAwait(false);
            if (selectedInstallation is null)
            {
                return new RuntimeReadinessResult(
                    false,
                    null,
                    new[] { "The verified source-installation selection does not exist." });
            }

            var boundary = WriteBoundary.Create(
                selectedInstallation,
                _externalRoot,
                _canonicalizer);
            var layout = LocalDataLayout.Create(_externalRoot, boundary);
            return await new ModRuntimeReadinessService(
                    layout,
                    boundary,
                    _canonicalizer,
                    new FileHashService(),
                    new RuntimePointerStore(layout, boundary))
                .ValidateAsync(cancellationToken)
                .ConfigureAwait(false);
        },
        cancellationToken);

    public async Task<IReadOnlyList<SaveProfileCandidate>> DiscoverSaveProfilesAsync(
        CancellationToken cancellationToken)
    {
        var profiles = (await _saveProfileLocator.DiscoverAsync(cancellationToken))
            .ToDictionary(candidate => candidate.SteamId, StringComparer.Ordinal);
        var components = await CreateSaveComponentsAsync(cancellationToken);

        if (Directory.Exists(components.Layout.Saves))
        {
            foreach (var directory in Directory.EnumerateDirectories(components.Layout.Saves))
            {
                cancellationToken.ThrowIfCancellationRequested();
                var steamId = Path.GetFileName(directory);
                var attributes = File.GetAttributes(directory);
                if (!IsSteamId(steamId)
                    || (attributes & FileAttributes.ReparsePoint) != 0
                    || !_fileAccess.Exists(Path.Combine(directory, "DRAKS0005.rmm")))
                {
                    continue;
                }

                profiles[steamId] = new SaveProfileCandidate(steamId, string.Empty);
            }
        }

        try
        {
            var persisted = await components.SelectionStore.ReadAsync(cancellationToken);
            if (persisted is not null && !profiles.ContainsKey(persisted.SteamId))
            {
                profiles[persisted.SteamId] = persisted;
            }
        }
        catch (Exception exception) when (
            exception is IOException or UnauthorizedAccessException or JsonException or ArgumentException)
        {
            // A malformed optional selection cannot hide independently discovered profiles.
        }

        return profiles.Values
            .OrderBy(candidate => candidate.SteamId, StringComparer.Ordinal)
            .ToArray();
    }

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
                _externalRoot,
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
                SaveErrorCode.FirstCopyConfirmationRequired,
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

    public async Task<SafetyLaunchResult> LaunchModdedAsync(
        string steamId,
        CancellationToken cancellationToken)
    {
        try
        {
            var selectedInstallation = await InstallationSelectionStore
                .CreateReadOnly(_externalRoot, _canonicalizer)
                .ReadAsync(cancellationToken);
            if (selectedInstallation is null)
            {
                return SafetyLaunchResult.Failed("SOURCE_INSTALLATION_NOT_SELECTED");
            }

            var boundary = WriteBoundary.Create(
                selectedInstallation,
                _externalRoot,
                _canonicalizer);
            var layout = LocalDataLayout.Create(_externalRoot, boundary);
            var pointerStore = new RuntimePointerStore(layout, boundary);
            var readiness = await new ModRuntimeReadinessService(
                    layout,
                    boundary,
                    _canonicalizer,
                    new FileHashService(),
                    pointerStore)
                .ValidateAsync(cancellationToken);
            if (!readiness.IsReady || string.IsNullOrWhiteSpace(readiness.RuntimePath))
            {
                return SafetyLaunchResult.Failed("MOD_RUNTIME_NOT_READY");
            }

            var runtimeRoot = _canonicalizer.Canonicalize(readiness.RuntimePath);
            if (runtimeRoot.Equals(
                    _canonicalizer.Canonicalize(selectedInstallation),
                    StringComparison.OrdinalIgnoreCase))
            {
                return SafetyLaunchResult.Failed("RUNTIME_SOURCE_NOT_SEPARATE");
            }

            var executablePath = Path.Combine(runtimeRoot, "DarkSoulsRemastered.exe");
            var steamModulePath = Path.Combine(runtimeRoot, "steam_api64.dll");
            using var profileArtifact = LaunchArtifactLease.TryOpen(_profilePath);
            if (profileArtifact is null
                || !profileArtifact.Sha256.Equals(
                    _artifactIdentities.ProfileSha256,
                    StringComparison.OrdinalIgnoreCase))
            {
                return SafetyLaunchResult.Failed("PROFILE_ARTIFACT_INVALID");
            }
            using var executableArtifact = LaunchArtifactLease.TryOpen(executablePath);
            using var steamArtifact = LaunchArtifactLease.TryOpen(steamModulePath);
            if (executableArtifact is null || steamArtifact is null)
            {
                return SafetyLaunchResult.Failed("GAME_PROFILE_MISMATCH");
            }
            using var guardArtifact = LaunchArtifactLease.TryOpen(_guardDllPath);
            if (guardArtifact is null
                || !guardArtifact.Sha256.Equals(
                    _artifactIdentities.GuardSha256,
                    StringComparison.OrdinalIgnoreCase))
            {
                return SafetyLaunchResult.Failed("GUARD_ARTIFACT_INVALID");
            }

            var profiles = _profiles ?? CompatibilityProfileCatalog.LoadJson(
                Encoding.UTF8.GetString(profileArtifact.Bytes));
            CompatibilityProfile profile;
            try
            {
                profile = profiles.Select(ProfileInspector.InspectIdentity(executableArtifact.Bytes));
            }
            catch (UnsupportedGameBuildException)
            {
                return SafetyLaunchResult.Failed("GAME_PROFILE_UNSUPPORTED");
            }
            catch (BadImageFormatException)
            {
                return SafetyLaunchResult.Failed("GAME_PROFILE_UNSUPPORTED");
            }

            var profileVerification = ProfileInspector.VerifyFiles(
                executableArtifact.Bytes,
                new Dictionary<string, byte[]>(StringComparer.OrdinalIgnoreCase)
                {
                    ["steam_api64.dll"] = steamArtifact.Bytes
                },
                profile);
            if (profileVerification.Error != ProfileError.None)
            {
                return SafetyLaunchResult.Failed("GAME_PROFILE_MISMATCH");
            }

            var selectionStore = new SaveSelectionStore(layout, boundary);
            var expectedDedicatedPath = SavePaths.GetDedicatedSave(
                _externalRoot,
                steamId,
                boundary);
            var virtualDocuments = layout.VirtualProfile;
            var virtualProfile = Path.Combine(
                virtualDocuments,
                "NBGI",
                "DARK SOULS REMASTERED",
                steamId);
            var virtualLogicalSave = Path.Combine(
                virtualProfile,
                "DRAKS0005.sl2");
            boundary.EnsureAllowed(virtualDocuments);
            boundary.EnsureAllowed(virtualProfile);
            boundary.EnsureAllowed(virtualLogicalSave);
            try
            {
                VirtualSaveLinkBinding.RemoveStaleAlias(
                    layout,
                    boundary,
                    _fileAccess,
                    virtualLogicalSave,
                    expectedDedicatedPath);
            }
            catch (VirtualSaveAliasConflictException)
            {
                return SafetyLaunchResult.Failed("DEDICATED_SAVE_ALIAS_CONFLICT");
            }
            if (!_fileAccess.Exists(expectedDedicatedPath))
            {
                var selection = await selectionStore.ReadAsync(cancellationToken);
                if (selection is null
                    || !selection.SteamId.Equals(steamId, StringComparison.Ordinal))
                {
                    var candidates = await _saveProfileLocator.DiscoverAsync(cancellationToken);
                    selection = candidates.SingleOrDefault(candidate =>
                        candidate.SteamId.Equals(steamId, StringComparison.Ordinal));
                    if (selection is null)
                    {
                        return SafetyLaunchResult.Failed("DEDICATED_SAVE_SOURCEMISSING");
                    }
                    await selectionStore.WriteAsync(selection, cancellationToken);
                }
            }
            var dedicatedSaveService = new DedicatedSaveService(
                layout,
                boundary,
                selectionStore,
                _fileAccess);
            var dedicatedSave = await dedicatedSaveService.PrepareAsync(
                steamId,
                cancellationToken);
            if (!dedicatedSave.Ready
                || string.IsNullOrWhiteSpace(dedicatedSave.SavePath)
                || string.IsNullOrWhiteSpace(dedicatedSave.SaveIdentity)
                || string.IsNullOrWhiteSpace(dedicatedSave.MetadataIdentity))
            {
                return SafetyLaunchResult.Failed(
                    $"DEDICATED_SAVE_{dedicatedSave.ErrorCode.ToString().ToUpperInvariant()}");
            }
            var sessionSaveService = new DedicatedSaveService(
                layout,
                boundary,
                selectionStore,
                new NormalSaveBlockingFileAccess(_fileAccess));

            var dedicatedPath = Path.GetFullPath(dedicatedSave.SavePath);
            var externalSaveRoot = Path.GetDirectoryName(dedicatedPath)
                ?? throw new IOException("The dedicated save root could not be resolved.");
            boundary.EnsureAllowed(externalSaveRoot);
            boundary.EnsureAllowed(dedicatedPath);
            var realSaveRoot = Path.GetFullPath(Path.Combine(
                _knownFolderProvider.GetDocumentsPath(),
                "NBGI",
                "DARK SOULS REMASTERED"));
            var savePaths = new GuardSavePathConfiguration(
                virtualDocuments,
                virtualLogicalSave,
                realSaveRoot,
                externalSaveRoot,
                dedicatedPath);
            var saveSession = await sessionSaveService.BeginSessionAsync(
                steamId,
                dedicatedSave.SaveIdentity,
                dedicatedSave.MetadataIdentity,
                cancellationToken);
            if (!saveSession.Ready || string.IsNullOrWhiteSpace(saveSession.SessionToken))
            {
                return SafetyLaunchResult.Failed("DEDICATED_SAVE_SESSION_UNAVAILABLE");
            }
            var request = new SafetyLaunchRequest(
                executablePath,
                runtimeRoot,
                _guardDllPath,
                profile,
                DedicatedSaveProtection.RequiredFlags,
                DiagnosticMode: false,
                Arguments: Array.Empty<string>(),
                SavePaths: savePaths);
            SafetyLaunchResult launchResult;
            try
            {
                launchResult = await new SafetyLaunchCoordinator(_platform)
                    .LaunchAsync(request, cancellationToken);
            }
            catch
            {
                await CompleteAbnormalSessionWithoutMaskingAsync(
                    sessionSaveService,
                    steamId,
                    saveSession.SessionToken);
                throw;
            }
            var normalGuardedExit = launchResult.Started && launchResult.ExitCode == 0;
            if (!normalGuardedExit)
            {
                await CompleteAbnormalSessionWithoutMaskingAsync(
                    sessionSaveService,
                    steamId,
                    saveSession.SessionToken);
                return launchResult;
            }
            var saveCompletion = await sessionSaveService.CompleteSessionAsync(
                steamId,
                saveSession.SessionToken,
                normalGuardedExit: true,
                CancellationToken.None);
            return !saveCompletion.Ready
                ? SafetyLaunchResult.Failed("DEDICATED_SAVE_SESSION_INVALID")
                : launchResult;
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (Exception exception) when (
            exception is IOException
                or UnauthorizedAccessException
                or ArgumentException
                or JsonException
                or CompatibilityProfileFormatException)
        {
            return SafetyLaunchResult.Failed("LAUNCH_PREFLIGHT_FAILED");
        }
    }

    private static async Task CompleteAbnormalSessionWithoutMaskingAsync(
        DedicatedSaveService service,
        string steamId,
        string sessionToken)
    {
        try
        {
            _ = await service.CompleteSessionAsync(
                steamId,
                sessionToken,
                normalGuardedExit: false,
                CancellationToken.None);
        }
        catch
        {
            // The original launch failure or cancellation remains authoritative.
        }
    }

    private async Task<SaveComponents> CreateSaveComponentsAsync(
        CancellationToken cancellationToken)
    {
        var selectedInstallation = await InstallationSelectionStore
            .CreateReadOnly(_externalRoot, _canonicalizer)
            .ReadAsync(cancellationToken);
        var protectedSource = selectedInstallation ?? Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
            "DSR-Randomizer-Protected-Source");
        var boundary = WriteBoundary.Create(
            protectedSource,
            _externalRoot,
            _canonicalizer);
        var layout = LocalDataLayout.Create(_externalRoot, boundary);
        return new SaveComponents(
            boundary,
            layout,
            new SaveSelectionStore(layout, boundary));
    }

    private static bool IsSteamId(string value) =>
        value.Length is >= 1 and <= 20
        && value.All(character => character is >= '0' and <= '9');

    private sealed record SaveComponents(
        WriteBoundary Boundary,
        LocalDataLayout Layout,
        SaveSelectionStore SelectionStore);
}

internal sealed class NormalSaveBlockingFileAccess(IFileAccess inner) : IFileAccess
{
    public bool Exists(string path) => IsNormalSave(path) ? false : inner.Exists(path);

    public IFileMutationLease AcquireMutationLease(
        string rootPath,
        IReadOnlyCollection<string> directoryPaths) =>
        inner.AcquireMutationLease(rootPath, directoryPaths);

    public IFileMutationLease AcquireSessionLock(string rootPath, string lockPath) =>
        inner.AcquireSessionLock(rootPath, lockPath);

    public FileAttributes GetAttributes(string path)
    {
        ThrowIfNormalSave(path);
        return inner.GetAttributes(path);
    }

    public bool IsSingleLinkFile(string path)
    {
        ThrowIfNormalSave(path);
        return inner.IsSingleLinkFile(path);
    }

    public Stream Open(string path, FileMode mode, FileAccess access, FileShare share)
    {
        ThrowIfNormalSave(path);
        return inner.Open(path, mode, access, share);
    }

    public Task<FileIdentityAndHash> IdentityAndHashAsync(
        Stream stream,
        CancellationToken cancellationToken) =>
        inner.IdentityAndHashAsync(stream, cancellationToken);

    public Task<FileIdentityAndHash> IdentityAndHashAsync(
        string path,
        CancellationToken cancellationToken)
    {
        ThrowIfNormalSave(path);
        return inner.IdentityAndHashAsync(path, cancellationToken);
    }

    public Task<CreatedFileIdentity> CopyAndFlushAsync(
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

    private static void ThrowIfNormalSave(string path)
    {
        if (IsNormalSave(path))
        {
            throw new UnauthorizedAccessException(
                "The normal DRAKS0005.sl2 save cannot be opened during guarded session setup or completion.");
        }
    }

    private static bool IsNormalSave(string path) =>
        Path.GetFileName(path).Equals("DRAKS0005.sl2", StringComparison.OrdinalIgnoreCase);
}
