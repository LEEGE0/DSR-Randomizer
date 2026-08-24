using System.Text.Json;
using DSRRandomizer.Foundation.Paths;

namespace DSRRandomizer.Foundation.Saves;

public sealed record DedicatedSaveSessionResult(
    DedicatedSaveResult Result,
    string? SessionToken)
{
    public bool Ready => Result.Ready;
    public bool ReusedExisting => Result.ReusedExisting;
    public string? SavePath => Result.SavePath;
    public SaveErrorCode ErrorCode => Result.ErrorCode;
    public string Message => Result.Message;
}

public sealed class DedicatedSaveService
{
    private const int MetadataSchemaVersion = 1;
    private const long FixedSaveLength = 4_326_608;
    private const string MetadataFileName = "save-metadata.json";
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase
    };

    private readonly LocalDataLayout _layout;
    private readonly WriteBoundary _boundary;
    private readonly SaveSelectionStore _selectionStore;
    private readonly IFileAccess _files;

    public DedicatedSaveService(
        LocalDataLayout layout,
        WriteBoundary boundary,
        SaveSelectionStore selectionStore,
        IFileAccess files)
    {
        ArgumentNullException.ThrowIfNull(layout);
        ArgumentNullException.ThrowIfNull(boundary);
        ArgumentNullException.ThrowIfNull(selectionStore);
        ArgumentNullException.ThrowIfNull(files);

        _layout = layout;
        _boundary = boundary;
        _selectionStore = selectionStore;
        _files = files;
    }

    public async Task<DedicatedSaveResult> PrepareAsync(
        string steamId,
        CancellationToken cancellationToken)
    {
        SaveLocations locations;
        try
        {
            locations = ResolveLocations(steamId);
        }
        catch (ArgumentException exception)
        {
            return DedicatedSaveResult.Fail(SaveErrorCode.InvalidSteamId, exception.Message);
        }
        catch (UnauthorizedAccessException exception)
        {
            return DedicatedSaveResult.Fail(SaveErrorCode.PathDenied, exception.Message);
        }
        catch (IOException exception)
        {
            return DedicatedSaveResult.Fail(SaveErrorCode.PathDenied, exception.Message);
        }

        if (_files.Exists(locations.DedicatedPath))
        {
            return await ValidateExistingAsync(locations, cancellationToken);
        }

        var selection = await ReadSelectedSourceAsync(steamId, cancellationToken);
        if (selection.Result is not null)
        {
            return selection.Result;
        }

        return await BootstrapAsync(locations, selection.SourcePath!, binding: null, cancellationToken);
    }

    public async Task<DedicatedSaveResult> ResetForSeedAsync(
        string steamId,
        SeedBinding binding,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(binding);
        if (string.IsNullOrWhiteSpace(binding.SeedId)
            || !IsSha256(binding.PlacementSha256))
        {
            return DedicatedSaveResult.Fail(SaveErrorCode.SeedMismatch, "Seed binding is incomplete.");
        }

        var prepared = await PrepareAsync(steamId, cancellationToken);
        if (!prepared.Ready)
        {
            return prepared;
        }

        SaveLocations locations;
        try
        {
            locations = ResolveLocations(steamId);
        }
        catch (UnauthorizedAccessException exception)
        {
            return DedicatedSaveResult.Fail(SaveErrorCode.PathDenied, exception.Message);
        }

        var selection = await ReadSelectedSourceAsync(steamId, cancellationToken);
        if (selection.Result is not null)
        {
            return selection.Result;
        }

        var stageResult = await StageSourceAsync(
            locations,
            selection.SourcePath!,
            cancellationToken);
        if (stageResult.Failure is not null)
        {
            return stageResult.Failure;
        }

        IFileMutationLease resetLease;
        try
        {
            resetLease = _files.AcquireMutationLease(
                _layout.Root,
                [locations.SaveDirectory, locations.ArchiveDirectory, _layout.Staging]);
            resetLease.Verify();
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            SafeDelete(stageResult.StagedFile);
            return DedicatedSaveResult.Fail(SaveErrorCode.PathDenied, exception.Message);
        }

        using (resetLease)
        {
            FileIdentityAndHash oldSaveSnapshot;
            FileIdentityAndHash oldMetadataSnapshot;
            try
            {
                oldSaveSnapshot = await _files.IdentityAndHashAsync(
                    locations.DedicatedPath,
                    cancellationToken);
                oldMetadataSnapshot = await _files.IdentityAndHashAsync(
                    locations.MetadataPath,
                    cancellationToken);
            }
            catch
            {
                SafeDelete(stageResult.StagedFile);
                throw;
            }

            var oldSave = new OwnedFile(locations.DedicatedPath, oldSaveSnapshot.Identity);
            var oldMetadata = new OwnedFile(locations.MetadataPath, oldMetadataSnapshot.Identity);

            var archiveId = $"{DateTime.UtcNow:yyyyMMddHHmmssfff}.{Guid.NewGuid():N}";
            var archiveSave = new OwnedFile(Path.Combine(
                locations.ArchiveDirectory,
                $"DRAKS0005.{archiveId}.rmm"), oldSave.Identity);
            var archiveMetadata = new OwnedFile(Path.Combine(
                locations.ArchiveDirectory,
                $"save-metadata.{archiveId}.json"), oldMetadata.Identity);
            var newSave = new OwnedFile(
                locations.DedicatedPath,
                stageResult.StagedFile!.Identity);
            OwnedFile? metadataTemporary = null;
            OwnedFile? newMetadata = null;

            try
            {
                EnsureExternal(locations.ArchiveDirectory, archiveSave.Path, archiveMetadata.Path);
                if (!_files.MoveCreateNewIfIdentityMatches(
                        oldMetadata.Path,
                        archiveMetadata.Path,
                        oldMetadata.Identity))
                {
                    throw new IOException("Existing metadata ownership changed before archive.");
                }

                cancellationToken.ThrowIfCancellationRequested();
                if (!_files.MoveCreateNewIfIdentityMatches(
                        oldSave.Path,
                        archiveSave.Path,
                        oldSave.Identity))
                {
                    throw new IOException("Existing save ownership changed before archive.");
                }

                cancellationToken.ThrowIfCancellationRequested();
                if (!_files.MoveCreateNewIfIdentityMatches(
                        stageResult.StagedFile.Path,
                        newSave.Path,
                        stageResult.StagedFile.Identity))
                {
                    throw new IOException("Staged save ownership changed before publication.");
                }

                cancellationToken.ThrowIfCancellationRequested();

                var metadata = CreateMetadata(steamId, stageResult.Source!, binding, cleanExit: true);
                metadataTemporary = await WriteMetadataTemporaryAsync(
                    locations,
                    metadata,
                    cancellationToken);
                newMetadata = new OwnedFile(locations.MetadataPath, metadataTemporary.Identity);
                if (!_files.ReplaceIfSourceIdentityMatches(
                        metadataTemporary.Path,
                        newMetadata.Path,
                        metadataTemporary.Identity))
                {
                    throw new IOException("Metadata temporary ownership changed before publication.");
                }

                cancellationToken.ThrowIfCancellationRequested();

                var published = await _files.IdentityAndHashAsync(
                    locations.DedicatedPath,
                    cancellationToken);
                if (!MatchesContent(stageResult.Source!, published))
                {
                    throw new CopyVerificationException("Published reset save failed verification.");
                }

                return Ready(locations.DedicatedPath, reused: false);
            }
            catch (Exception exception)
            {
                await RollBackResetAsync(
                    oldSave,
                    oldMetadata,
                    archiveSave,
                    archiveMetadata,
                    newSave,
                    newMetadata,
                    metadataTemporary);

                if (exception is OperationCanceledException)
                {
                    throw;
                }

                return DedicatedSaveResult.Fail(
                    exception is UnauthorizedAccessException
                        ? SaveErrorCode.PathDenied
                        : SaveErrorCode.CopyVerificationFailed,
                    exception.Message);
            }
            finally
            {
                SafeDelete(stageResult.StagedFile);
                SafeDelete(metadataTemporary);
            }
        }
    }

    public async Task<DedicatedSaveSessionResult> BeginSessionAsync(
        string steamId,
        CancellationToken cancellationToken)
    {
        var prepared = await PrepareAsync(steamId, cancellationToken);
        if (!prepared.Ready)
        {
            return new DedicatedSaveSessionResult(prepared, null);
        }

        OwnedFile? sessionTemporary = null;
        OwnedFile? publishedSession = null;
        try
        {
            var locations = ResolveLocations(steamId);
            using var sessionLease = _files.AcquireMutationLease(
                _layout.Root,
                [locations.SaveDirectory]);
            sessionLease.Verify();
            var metadata = await ReadMetadataAsync(locations.MetadataPath, cancellationToken);
            if (metadata is null)
            {
                return new DedicatedSaveSessionResult(
                    DedicatedSaveResult.Fail(
                        SaveErrorCode.ExistingSaveInvalid,
                        "Dedicated save metadata is invalid."),
                    null);
            }

            var sessionToken = Guid.NewGuid().ToString("N");
            var sessionState = new DedicatedSaveSessionState(
                1,
                steamId,
                sessionToken);
            sessionTemporary = await WriteSessionTemporaryAsync(
                locations,
                sessionState,
                cancellationToken);
            publishedSession = new OwnedFile(
                locations.SessionStatePath,
                sessionTemporary.Identity);
            if (!_files.ReplaceIfSourceIdentityMatches(
                    sessionTemporary.Path,
                    publishedSession.Path,
                    sessionTemporary.Identity))
            {
                throw new IOException("Session temporary ownership changed before publication.");
            }

            await PublishMetadataAsync(
                locations,
                metadata with { CleanExit = false },
                cancellationToken);
            return new DedicatedSaveSessionResult(
                Ready(locations.DedicatedPath, prepared.ReusedExisting),
                sessionToken);
        }
        catch (OperationCanceledException)
        {
            SafeDelete(publishedSession);
            throw;
        }
        catch (UnauthorizedAccessException exception)
        {
            SafeDelete(publishedSession);
            return new DedicatedSaveSessionResult(
                DedicatedSaveResult.Fail(SaveErrorCode.PathDenied, exception.Message),
                null);
        }
        catch (Exception exception) when (exception is IOException or JsonException)
        {
            SafeDelete(publishedSession);
            return new DedicatedSaveSessionResult(
                DedicatedSaveResult.Fail(SaveErrorCode.ExistingSaveInvalid, exception.Message),
                null);
        }
        finally
        {
            SafeDelete(sessionTemporary);
        }
    }

    public async Task<DedicatedSaveResult> CompleteSessionAsync(
        string steamId,
        string sessionToken,
        bool normalGuardedExit,
        CancellationToken cancellationToken)
    {
        if (!normalGuardedExit)
        {
            return DedicatedSaveResult.Fail(
                SaveErrorCode.ExistingSaveInvalid,
                "The guarded session did not exit normally.");
        }

        try
        {
            var locations = ResolveLocations(steamId);
            using var sessionLease = _files.AcquireMutationLease(
                _layout.Root,
                [locations.SaveDirectory]);
            sessionLease.Verify();
            if (!_files.Exists(locations.DedicatedPath))
            {
                return DedicatedSaveResult.Fail(
                    SaveErrorCode.ExistingSaveInvalid,
                    "Dedicated save is missing.");
            }

            if (string.IsNullOrWhiteSpace(sessionToken)
                || !_files.Exists(locations.SessionStatePath))
            {
                return DedicatedSaveResult.Fail(
                    SaveErrorCode.ExistingSaveInvalid,
                    "No matching unclean session is active.");
            }

            var sessionState = await ReadSessionStateAsync(
                locations.SessionStatePath,
                cancellationToken);
            if (sessionState is null
                || sessionState.SchemaVersion != 1
                || !string.Equals(sessionState.SteamId, steamId, StringComparison.Ordinal)
                || !string.Equals(sessionState.SessionToken, sessionToken, StringComparison.Ordinal))
            {
                return DedicatedSaveResult.Fail(
                    SaveErrorCode.ExistingSaveInvalid,
                    "The session token is missing, invalid, or stale.");
            }

            var sessionIdentity = await _files.IdentityAndHashAsync(
                locations.SessionStatePath,
                cancellationToken);

            var metadata = await ReadMetadataAsync(locations.MetadataPath, cancellationToken);
            if (!MetadataShapeIsValid(metadata, steamId) || metadata!.CleanExit)
            {
                return DedicatedSaveResult.Fail(
                    SaveErrorCode.ExistingSaveInvalid,
                    "Dedicated save metadata is invalid.");
            }

            var snapshot = await _files.IdentityAndHashAsync(
                locations.DedicatedPath,
                cancellationToken);
            if (snapshot.Length != FixedSaveLength)
            {
                return DedicatedSaveResult.Fail(
                    SaveErrorCode.ExistingSaveInvalid,
                    "Dedicated save length changed.");
            }

            await PublishMetadataAsync(
                locations,
                metadata! with
                {
                    LastKnownSha256 = snapshot.Sha256,
                    CleanExit = true
                },
                cancellationToken);
            SafeDelete(new OwnedFile(
                locations.SessionStatePath,
                sessionIdentity.Identity));
            return Ready(locations.DedicatedPath, reused: true);
        }
        catch (UnauthorizedAccessException exception)
        {
            return DedicatedSaveResult.Fail(SaveErrorCode.PathDenied, exception.Message);
        }
        catch (Exception exception) when (exception is IOException or JsonException)
        {
            return DedicatedSaveResult.Fail(SaveErrorCode.ExistingSaveInvalid, exception.Message);
        }
    }

    private async Task<DedicatedSaveResult> BootstrapAsync(
        SaveLocations locations,
        string sourcePath,
        SeedBinding? binding,
        CancellationToken cancellationToken)
    {
        var stageResult = await StageSourceAsync(locations, sourcePath, cancellationToken);
        if (stageResult.Failure is not null)
        {
            return stageResult.Failure;
        }

        IFileMutationLease bootstrapLease;
        try
        {
            bootstrapLease = _files.AcquireMutationLease(
                _layout.Root,
                [locations.SaveDirectory, _layout.Staging]);
            bootstrapLease.Verify();
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            SafeDelete(stageResult.StagedFile);
            return DedicatedSaveResult.Fail(SaveErrorCode.PathDenied, exception.Message);
        }

        using (bootstrapLease)
        {

            var publishedSave = new OwnedFile(
                locations.DedicatedPath,
                stageResult.StagedFile!.Identity);
            OwnedFile? metadataTemporary = null;
            OwnedFile? publishedMetadata = null;
            try
            {
                EnsureExternal(locations.SaveDirectory, locations.DedicatedPath, locations.MetadataPath);
                try
                {
                    if (!_files.MoveCreateNewIfIdentityMatches(
                            stageResult.StagedFile.Path,
                            locations.DedicatedPath,
                            stageResult.StagedFile.Identity))
                    {
                        throw new IOException("Staged save ownership changed before publication.");
                    }
                }
                catch (IOException exception)
                {
                    if (await IsOwnedAtAsync(publishedSave))
                    {
                        throw;
                    }

                    return DedicatedSaveResult.Fail(SaveErrorCode.DestinationRace, exception.Message);
                }

                cancellationToken.ThrowIfCancellationRequested();

                var metadata = CreateMetadata(
                    locations.SteamId,
                    stageResult.Source!,
                    binding,
                    cleanExit: true);
                metadataTemporary = await WriteMetadataTemporaryAsync(
                    locations,
                    metadata,
                    cancellationToken);
                publishedMetadata = new OwnedFile(
                    locations.MetadataPath,
                    metadataTemporary.Identity);
                if (!_files.ReplaceIfSourceIdentityMatches(
                        metadataTemporary.Path,
                        publishedMetadata.Path,
                        metadataTemporary.Identity))
                {
                    throw new IOException("Metadata temporary ownership changed before publication.");
                }

                cancellationToken.ThrowIfCancellationRequested();

                var published = await _files.IdentityAndHashAsync(
                    locations.DedicatedPath,
                    cancellationToken);
                if (!MatchesContent(stageResult.Source!, published))
                {
                    throw new CopyVerificationException("Published save failed verification.");
                }

                return Ready(locations.DedicatedPath, reused: false);
            }
            catch (OperationCanceledException)
            {
                SafeDelete(publishedMetadata);
                SafeDelete(metadataTemporary);
                SafeDelete(publishedSave);
                throw;
            }
            catch (Exception exception)
            {
                SafeDelete(publishedMetadata);
                SafeDelete(metadataTemporary);
                SafeDelete(publishedSave);
                return DedicatedSaveResult.Fail(
                    exception is UnauthorizedAccessException
                        ? SaveErrorCode.PathDenied
                        : SaveErrorCode.CopyVerificationFailed,
                    exception.Message);
            }
            finally
            {
                SafeDelete(stageResult.StagedFile);
                SafeDelete(metadataTemporary);
            }
        }
    }

    private async Task<StageResult> StageSourceAsync(
        SaveLocations locations,
        string sourcePath,
        CancellationToken cancellationToken)
    {
        var stagedPath = Path.Combine(
            _layout.Staging,
            $"DRAKS0005.{locations.SteamId}.{Guid.NewGuid():N}.stage");
        var stagedForCaller = false;

        try
        {
            EnsureExternal(_layout.Staging, stagedPath);
            using var stagingLease = _files.AcquireMutationLease(
                _layout.Root,
                [_layout.Staging]);
            stagingLease.Verify();
            if (!_files.Exists(sourcePath))
            {
                return StageResult.Fail(SaveErrorCode.SourceMissing, "Selected normal save is missing.");
            }

            await using var source = _files.Open(
                sourcePath,
                FileMode.Open,
                FileAccess.Read,
                FileShare.Read);
            var before = await _files.IdentityAndHashAsync(source, cancellationToken);
            if (before.Length != FixedSaveLength)
            {
                return StageResult.Fail(
                    SaveErrorCode.CopyVerificationFailed,
                    "Selected normal save has an unsupported length.");
            }

            await _files.CopyAndFlushAsync(source, stagedPath, cancellationToken);
            var staged = await _files.IdentityAndHashAsync(stagedPath, cancellationToken);
            if (!MatchesContent(before, staged))
            {
                return StageResult.Fail(
                    SaveErrorCode.CopyVerificationFailed,
                    "Staged save does not match the selected source.");
            }

            var after = await _files.IdentityAndHashAsync(source, cancellationToken);
            if (!before.Identity.Equals(after.Identity, StringComparison.Ordinal)
                || before.Length != after.Length
                || before.LastWriteTimeUtc != after.LastWriteTimeUtc)
            {
                return StageResult.Fail(
                    SaveErrorCode.SourceChanged,
                    "Selected normal save changed during bootstrap.");
            }

            stagedForCaller = true;
            return new StageResult(
                new OwnedFile(stagedPath, staged.Identity),
                before,
                null);
        }
        catch (FileNotFoundException exception)
        {
            return StageResult.Fail(SaveErrorCode.SourceMissing, exception.Message);
        }
        catch (UnauthorizedAccessException exception)
        {
            return StageResult.Fail(SaveErrorCode.PathDenied, exception.Message);
        }
        catch (IOException exception)
        {
            return StageResult.Fail(SaveErrorCode.CopyVerificationFailed, exception.Message);
        }
        finally
        {
            if (!stagedForCaller)
            {
                await DeleteUniqueFileIfPresentAsync(stagedPath);
            }
        }
    }

    private async Task<DedicatedSaveResult> ValidateExistingAsync(
        SaveLocations locations,
        CancellationToken cancellationToken)
    {
        try
        {
            EnsureExternal(locations.SaveDirectory, locations.DedicatedPath, locations.MetadataPath);
            if (!IsRegularFile(locations.DedicatedPath) || !IsRegularFile(locations.MetadataPath))
            {
                return DedicatedSaveResult.Fail(
                    SaveErrorCode.ExistingSaveInvalid,
                    "Dedicated save or metadata is not a regular external file.");
            }

            var metadata = await ReadMetadataAsync(locations.MetadataPath, cancellationToken);
            if (!MetadataShapeIsValid(metadata, locations.SteamId))
            {
                return DedicatedSaveResult.Fail(
                    SaveErrorCode.ExistingSaveInvalid,
                    "Dedicated save metadata is invalid.");
            }

            var snapshot = await _files.IdentityAndHashAsync(
                locations.DedicatedPath,
                cancellationToken);
            if (snapshot.Length != FixedSaveLength
                || snapshot.Length != metadata!.FixedLength
                || (metadata.CleanExit
                    && !snapshot.Sha256.Equals(
                        metadata.LastKnownSha256,
                        StringComparison.OrdinalIgnoreCase)))
            {
                return DedicatedSaveResult.Fail(
                    SaveErrorCode.ExistingSaveInvalid,
                    "Dedicated save content does not match metadata.");
            }

            return Ready(locations.DedicatedPath, reused: true);
        }
        catch (UnauthorizedAccessException exception)
        {
            return DedicatedSaveResult.Fail(SaveErrorCode.PathDenied, exception.Message);
        }
        catch (Exception exception) when (exception is IOException or JsonException)
        {
            return DedicatedSaveResult.Fail(SaveErrorCode.ExistingSaveInvalid, exception.Message);
        }
    }

    private async Task<(string? SourcePath, DedicatedSaveResult? Result)> ReadSelectedSourceAsync(
        string steamId,
        CancellationToken cancellationToken)
    {
        try
        {
            var selection = await _selectionStore.ReadAsync(cancellationToken);
            if (selection is null)
            {
                return (null, DedicatedSaveResult.Fail(
                    SaveErrorCode.SourceMissing,
                    "No normal save profile has been selected."));
            }

            if (!selection.SteamId.Equals(steamId, StringComparison.Ordinal))
            {
                return (null, DedicatedSaveResult.Fail(
                    SaveErrorCode.SourceMissing,
                    "The selected normal save belongs to another Steam ID."));
            }

            return (selection.SourcePath, null);
        }
        catch (UnauthorizedAccessException exception)
        {
            return (null, DedicatedSaveResult.Fail(SaveErrorCode.PathDenied, exception.Message));
        }
        catch (Exception exception) when (exception is IOException or JsonException or ArgumentException)
        {
            return (null, DedicatedSaveResult.Fail(SaveErrorCode.SourceMissing, exception.Message));
        }
    }

    private async Task<DedicatedSaveMetadata?> ReadMetadataAsync(
        string metadataPath,
        CancellationToken cancellationToken)
    {
        if (!_files.Exists(metadataPath))
        {
            return null;
        }

        await using var stream = _files.Open(
            metadataPath,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read);
        return await JsonSerializer.DeserializeAsync<DedicatedSaveMetadata>(
            stream,
            JsonOptions,
            cancellationToken);
    }

    private async Task<DedicatedSaveSessionState?> ReadSessionStateAsync(
        string sessionStatePath,
        CancellationToken cancellationToken)
    {
        await using var stream = _files.Open(
            sessionStatePath,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read);
        return await JsonSerializer.DeserializeAsync<DedicatedSaveSessionState>(
            stream,
            JsonOptions,
            cancellationToken);
    }

    private async Task RollBackResetAsync(
        OwnedFile oldSave,
        OwnedFile oldMetadata,
        OwnedFile archiveSave,
        OwnedFile archiveMetadata,
        OwnedFile newSave,
        OwnedFile? newMetadata,
        OwnedFile? metadataTemporary)
    {
        SafeDelete(newMetadata);
        SafeDelete(metadataTemporary);
        SafeDelete(newSave);

        var oldSaveLive = await IsOwnedAtAsync(oldSave);
        var oldMetadataLive = await IsOwnedAtAsync(oldMetadata);
        if (oldSaveLive && oldMetadataLive)
        {
            return;
        }

        var savePathOccupiedByForeign = _files.Exists(oldSave.Path) && !oldSaveLive;
        var metadataPathOccupiedByForeign = _files.Exists(oldMetadata.Path) && !oldMetadataLive;

        if (oldSaveLive)
        {
            await TryMoveOwnedAsync(oldSave, archiveSave.Path);
        }

        if (oldMetadataLive)
        {
            await TryMoveOwnedAsync(oldMetadata, archiveMetadata.Path);
        }

        if (savePathOccupiedByForeign || metadataPathOccupiedByForeign)
        {
            return;
        }

        if (!await IsOwnedAtAsync(archiveSave)
            || !await IsOwnedAtAsync(archiveMetadata)
            || _files.Exists(oldSave.Path)
            || _files.Exists(oldMetadata.Path))
        {
            return;
        }

        if (!await TryMoveOwnedAsync(archiveMetadata, oldMetadata.Path))
        {
            return;
        }

        var restoredMetadata = new OwnedFile(oldMetadata.Path, oldMetadata.Identity);
        if (await TryMoveOwnedAsync(archiveSave, oldSave.Path))
        {
            return;
        }

        await TryMoveOwnedAsync(restoredMetadata, archiveMetadata.Path);
    }

    private async Task<bool> TryMoveOwnedAsync(OwnedFile source, string destinationPath)
    {
        try
        {
            if (!await IsOwnedAtAsync(source))
            {
                return false;
            }

            return _files.MoveCreateNewIfIdentityMatches(
                source.Path,
                destinationPath,
                source.Identity);
        }
        catch (Exception exception) when (
            exception is IOException or UnauthorizedAccessException or FileNotFoundException)
        {
            return false;
        }
    }

    private async Task<bool> IsOwnedAtAsync(OwnedFile ownedFile)
    {
        try
        {
            if (!_files.Exists(ownedFile.Path))
            {
                return false;
            }

            var identity = await _files.IdentityAndHashAsync(
                ownedFile.Path,
                CancellationToken.None);
            return identity.Identity.Equals(ownedFile.Identity, StringComparison.Ordinal);
        }
        catch (Exception exception) when (
            exception is IOException or UnauthorizedAccessException or FileNotFoundException)
        {
            return false;
        }
    }

    private async Task<OwnedFile> PublishMetadataAsync(
        SaveLocations locations,
        DedicatedSaveMetadata metadata,
        CancellationToken cancellationToken)
    {
        var temporary = await WriteMetadataTemporaryAsync(
            locations,
            metadata,
            cancellationToken);
        try
        {
            if (!_files.ReplaceIfSourceIdentityMatches(
                    temporary.Path,
                    locations.MetadataPath,
                    temporary.Identity))
            {
                throw new IOException("Metadata temporary file ownership changed before publication.");
            }

            return new OwnedFile(locations.MetadataPath, temporary.Identity);
        }
        finally
        {
            SafeDelete(temporary);
        }
    }

    private async Task<OwnedFile> WriteMetadataTemporaryAsync(
        SaveLocations locations,
        DedicatedSaveMetadata metadata,
        CancellationToken cancellationToken)
    {
        var temporaryPath = Path.Combine(
            locations.SaveDirectory,
            $"save-metadata.{Guid.NewGuid():N}.tmp");
        EnsureExternal(locations.SaveDirectory, temporaryPath, locations.MetadataPath);
        var bytes = JsonSerializer.SerializeToUtf8Bytes(metadata, JsonOptions);
        var created = await _files.WriteAllBytesAndFlushAsync(
            temporaryPath,
            bytes,
            cancellationToken);
        return new OwnedFile(temporaryPath, created.Identity);
    }

    private async Task<OwnedFile> WriteSessionTemporaryAsync(
        SaveLocations locations,
        DedicatedSaveSessionState sessionState,
        CancellationToken cancellationToken)
    {
        var temporaryPath = Path.Combine(
            locations.SaveDirectory,
            $"session-state.{Guid.NewGuid():N}.tmp");
        EnsureExternal(
            locations.SaveDirectory,
            temporaryPath,
            locations.SessionStatePath);
        var bytes = JsonSerializer.SerializeToUtf8Bytes(sessionState, JsonOptions);
        var created = await _files.WriteAllBytesAndFlushAsync(
            temporaryPath,
            bytes,
            cancellationToken);
        return new OwnedFile(temporaryPath, created.Identity);
    }

    private SaveLocations ResolveLocations(string steamId)
    {
        var dedicatedPath = SavePaths.GetDedicatedSave(_layout.Root, steamId, _boundary);
        var saveDirectory = Path.GetDirectoryName(dedicatedPath)
            ?? throw new IOException("Dedicated save directory could not be resolved.");
        var metadataPath = Path.Combine(saveDirectory, MetadataFileName);
        var sessionStatePath = Path.Combine(saveDirectory, "session-state.json");
        var archiveDirectory = Path.Combine(saveDirectory, "archive");
        EnsureExternal(
            saveDirectory,
            dedicatedPath,
            metadataPath,
            sessionStatePath,
            archiveDirectory,
            _layout.Staging);
        return new SaveLocations(
            steamId,
            saveDirectory,
            dedicatedPath,
            metadataPath,
            sessionStatePath,
            archiveDirectory);
    }

    private void EnsureExternal(params string[] paths)
    {
        foreach (var path in paths)
        {
            _boundary.EnsureAllowed(path);
        }
    }

    private bool IsRegularFile(string path)
    {
        if (!_files.Exists(path))
        {
            return false;
        }

        var attributes = _files.GetAttributes(path);
        return (attributes & (FileAttributes.Directory | FileAttributes.ReparsePoint)) == 0;
    }

    private static bool MetadataShapeIsValid(
        DedicatedSaveMetadata? metadata,
        string steamId) =>
        metadata is not null
        && metadata.SchemaVersion == MetadataSchemaVersion
        && string.Equals(metadata.SteamId, steamId, StringComparison.Ordinal)
        && metadata.FixedLength == FixedSaveLength
        && IsSha256(metadata.LastKnownSha256)
        && ((metadata.ActiveSeedId is null && metadata.PlacementSha256 is null)
            || (!string.IsNullOrWhiteSpace(metadata.ActiveSeedId)
                && IsSha256(metadata.PlacementSha256)));

    private static bool IsSha256(string? value) =>
        value is { Length: 64 }
        && value.All(Uri.IsHexDigit);

    private static DedicatedSaveMetadata CreateMetadata(
        string steamId,
        FileIdentityAndHash snapshot,
        SeedBinding? binding,
        bool cleanExit) =>
        new(
            MetadataSchemaVersion,
            steamId,
            FixedSaveLength,
            snapshot.Sha256,
            binding?.SeedId,
            binding?.PlacementSha256,
            cleanExit);

    private static bool MatchesContent(
        FileIdentityAndHash expected,
        FileIdentityAndHash actual) =>
        expected.Length == actual.Length
        && expected.Sha256.Equals(actual.Sha256, StringComparison.OrdinalIgnoreCase);

    private static DedicatedSaveResult Ready(string path, bool reused) =>
        new(true, reused, path, SaveErrorCode.None, string.Empty);

    private void SafeDelete(OwnedFile? ownedFile)
    {
        if (ownedFile is null)
        {
            return;
        }

        try
        {
            _boundary.EnsureAllowed(ownedFile.Path);
            _files.DeleteIfIdentityMatches(ownedFile.Path, ownedFile.Identity);
        }
        catch (Exception exception) when (
            exception is IOException or UnauthorizedAccessException or FileNotFoundException)
        {
            // Cleanup never widens the transaction or replaces the primary failure.
        }
    }

    private async Task DeleteUniqueFileIfPresentAsync(string path)
    {
        try
        {
            _boundary.EnsureAllowed(path);
            if (!_files.Exists(path))
            {
                return;
            }

            var identity = await _files.IdentityAndHashAsync(path, CancellationToken.None);
            _files.DeleteIfIdentityMatches(path, identity.Identity);
        }
        catch (Exception exception) when (
            exception is IOException or UnauthorizedAccessException or FileNotFoundException)
        {
            // A missing or replaced unique file is not transaction-owned cleanup work.
        }
    }

    private sealed record SaveLocations(
        string SteamId,
        string SaveDirectory,
        string DedicatedPath,
        string MetadataPath,
        string SessionStatePath,
        string ArchiveDirectory);

    private sealed record DedicatedSaveSessionState(
        int SchemaVersion,
        string SteamId,
        string SessionToken);

    private sealed record OwnedFile(string Path, string Identity);

    private sealed record StageResult(
        OwnedFile? StagedFile,
        FileIdentityAndHash? Source,
        DedicatedSaveResult? Failure)
    {
        public static StageResult Fail(SaveErrorCode code, string message) =>
            new(null, null, DedicatedSaveResult.Fail(code, message));
    }

    private sealed class CopyVerificationException(string message) : IOException(message);
}
