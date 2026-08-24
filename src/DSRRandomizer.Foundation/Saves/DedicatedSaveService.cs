using System.Text.Json;
using DSRRandomizer.Foundation.Paths;

namespace DSRRandomizer.Foundation.Saves;

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
            || string.IsNullOrWhiteSpace(binding.PlacementSha256))
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

        var archiveId = $"{DateTime.UtcNow:yyyyMMddHHmmssfff}.{Guid.NewGuid():N}";
        var archivePath = Path.Combine(
            locations.ArchiveDirectory,
            $"DRAKS0005.{archiveId}.rmm");
        var metadataArchivePath = Path.Combine(
            locations.ArchiveDirectory,
            $"save-metadata.{archiveId}.json");
        string? metadataTemporaryPath = null;
        var archived = false;
        var metadataArchived = false;
        var newDestinationPublished = false;
        var newMetadataPublished = false;

        try
        {
            EnsureExternal(locations.ArchiveDirectory, archivePath, metadataArchivePath);
            _files.CreateDirectory(locations.ArchiveDirectory);
            _files.MoveCreateNew(locations.DedicatedPath, archivePath);
            archived = true;
            _files.MoveCreateNew(locations.MetadataPath, metadataArchivePath);
            metadataArchived = true;
            _files.MoveCreateNew(stageResult.StagedPath!, locations.DedicatedPath);
            newDestinationPublished = true;

            var metadata = CreateMetadata(steamId, stageResult.Source!, binding, cleanExit: true);
            metadataTemporaryPath = await WriteMetadataTemporaryAsync(
                locations,
                metadata,
                cancellationToken);
            _files.Replace(metadataTemporaryPath, locations.MetadataPath);
            metadataTemporaryPath = null;
            newMetadataPublished = true;

            var published = await _files.IdentityAndHashAsync(
                locations.DedicatedPath,
                cancellationToken);
            if (!MatchesContent(stageResult.Source!, published))
            {
                throw new CopyVerificationException("Published reset save failed verification.");
            }

            return Ready(locations.DedicatedPath, reused: false);
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            if (newDestinationPublished && _files.Exists(locations.DedicatedPath))
            {
                SafeDelete(locations.DedicatedPath);
            }

            if (newMetadataPublished && _files.Exists(locations.MetadataPath))
            {
                SafeDelete(locations.MetadataPath);
            }

            if (archived && _files.Exists(archivePath) && !_files.Exists(locations.DedicatedPath))
            {
                try
                {
                    _files.MoveCreateNew(archivePath, locations.DedicatedPath);
                }
                catch (IOException)
                {
                    // Preserve the archive if an external race prevents restoration.
                }
            }

            if (metadataArchived
                && _files.Exists(metadataArchivePath)
                && !_files.Exists(locations.MetadataPath))
            {
                try
                {
                    _files.MoveCreateNew(metadataArchivePath, locations.MetadataPath);
                }
                catch (IOException)
                {
                    // Preserve the archive if an external race prevents restoration.
                }
            }

            return DedicatedSaveResult.Fail(
                exception is UnauthorizedAccessException
                    ? SaveErrorCode.PathDenied
                    : SaveErrorCode.CopyVerificationFailed,
                exception.Message);
        }
        finally
        {
            SafeDelete(stageResult.StagedPath);
            SafeDelete(metadataTemporaryPath);
        }
    }

    public async Task<DedicatedSaveResult> BeginSessionAsync(
        string steamId,
        CancellationToken cancellationToken)
    {
        var prepared = await PrepareAsync(steamId, cancellationToken);
        if (!prepared.Ready)
        {
            return prepared;
        }

        try
        {
            var locations = ResolveLocations(steamId);
            var metadata = await ReadMetadataAsync(locations.MetadataPath, cancellationToken);
            if (metadata is null)
            {
                return DedicatedSaveResult.Fail(
                    SaveErrorCode.ExistingSaveInvalid,
                    "Dedicated save metadata is invalid.");
            }

            await PublishMetadataAsync(
                locations,
                metadata with { CleanExit = false },
                cancellationToken);
            return Ready(locations.DedicatedPath, prepared.ReusedExisting);
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

    public async Task<DedicatedSaveResult> CompleteSessionAsync(
        string steamId,
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
            if (!_files.Exists(locations.DedicatedPath))
            {
                return DedicatedSaveResult.Fail(
                    SaveErrorCode.ExistingSaveInvalid,
                    "Dedicated save is missing.");
            }

            var metadata = await ReadMetadataAsync(locations.MetadataPath, cancellationToken);
            if (!MetadataShapeIsValid(metadata, steamId))
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

        string? metadataTemporaryPath = null;
        try
        {
            EnsureExternal(locations.SaveDirectory, locations.DedicatedPath, locations.MetadataPath);
            _files.CreateDirectory(locations.SaveDirectory);
            try
            {
                _files.MoveCreateNew(stageResult.StagedPath!, locations.DedicatedPath);
            }
            catch (IOException exception) when (_files.Exists(locations.DedicatedPath))
            {
                return DedicatedSaveResult.Fail(SaveErrorCode.DestinationRace, exception.Message);
            }

            var metadata = CreateMetadata(
                locations.SteamId,
                stageResult.Source!,
                binding,
                cleanExit: true);
            metadataTemporaryPath = await WriteMetadataTemporaryAsync(
                locations,
                metadata,
                cancellationToken);
            _files.Replace(metadataTemporaryPath, locations.MetadataPath);
            metadataTemporaryPath = null;

            var published = await _files.IdentityAndHashAsync(
                locations.DedicatedPath,
                cancellationToken);
            if (!MatchesContent(stageResult.Source!, published))
            {
                return DedicatedSaveResult.Fail(
                    SaveErrorCode.CopyVerificationFailed,
                    "Published save failed verification.");
            }

            return Ready(locations.DedicatedPath, reused: false);
        }
        catch (UnauthorizedAccessException exception)
        {
            return DedicatedSaveResult.Fail(SaveErrorCode.PathDenied, exception.Message);
        }
        catch (Exception exception) when (exception is IOException or JsonException)
        {
            return DedicatedSaveResult.Fail(SaveErrorCode.CopyVerificationFailed, exception.Message);
        }
        finally
        {
            SafeDelete(stageResult.StagedPath);
            SafeDelete(metadataTemporaryPath);
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
            _files.CreateDirectory(_layout.Staging);
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
            return new StageResult(stagedPath, before, null);
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
                SafeDelete(stagedPath);
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

    private async Task PublishMetadataAsync(
        SaveLocations locations,
        DedicatedSaveMetadata metadata,
        CancellationToken cancellationToken)
    {
        var temporaryPath = await WriteMetadataTemporaryAsync(
            locations,
            metadata,
            cancellationToken);
        try
        {
            _files.Replace(temporaryPath, locations.MetadataPath);
        }
        finally
        {
            SafeDelete(temporaryPath);
        }
    }

    private async Task<string> WriteMetadataTemporaryAsync(
        SaveLocations locations,
        DedicatedSaveMetadata metadata,
        CancellationToken cancellationToken)
    {
        var temporaryPath = Path.Combine(
            locations.SaveDirectory,
            $"save-metadata.{Guid.NewGuid():N}.tmp");
        EnsureExternal(locations.SaveDirectory, temporaryPath, locations.MetadataPath);
        _files.CreateDirectory(locations.SaveDirectory);
        var bytes = JsonSerializer.SerializeToUtf8Bytes(metadata, JsonOptions);
        await _files.WriteAllBytesAndFlushAsync(temporaryPath, bytes, cancellationToken);
        return temporaryPath;
    }

    private SaveLocations ResolveLocations(string steamId)
    {
        var dedicatedPath = SavePaths.GetDedicatedSave(_layout.Root, steamId, _boundary);
        var saveDirectory = Path.GetDirectoryName(dedicatedPath)
            ?? throw new IOException("Dedicated save directory could not be resolved.");
        var metadataPath = Path.Combine(saveDirectory, MetadataFileName);
        var archiveDirectory = Path.Combine(saveDirectory, "archive");
        EnsureExternal(saveDirectory, dedicatedPath, metadataPath, archiveDirectory, _layout.Staging);
        return new SaveLocations(
            steamId,
            saveDirectory,
            dedicatedPath,
            metadataPath,
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
        && metadata.SteamId.Equals(steamId, StringComparison.Ordinal)
        && metadata.FixedLength == FixedSaveLength
        && !string.IsNullOrWhiteSpace(metadata.LastKnownSha256)
        && metadata.LastKnownSha256.Length == 64
        && metadata.LastKnownSha256.All(Uri.IsHexDigit)
        && ((metadata.ActiveSeedId is null && metadata.PlacementSha256 is null)
            || (!string.IsNullOrWhiteSpace(metadata.ActiveSeedId)
                && !string.IsNullOrWhiteSpace(metadata.PlacementSha256)));

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

    private void SafeDelete(string? path)
    {
        if (path is null)
        {
            return;
        }

        try
        {
            _boundary.EnsureAllowed(path);
            if (_files.Exists(path))
            {
                _files.Delete(path);
            }
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            // Cleanup never widens the transaction or replaces the primary failure.
        }
    }

    private sealed record SaveLocations(
        string SteamId,
        string SaveDirectory,
        string DedicatedPath,
        string MetadataPath,
        string ArchiveDirectory);

    private sealed record StageResult(
        string? StagedPath,
        FileIdentityAndHash? Source,
        DedicatedSaveResult? Failure)
    {
        public static StageResult Fail(SaveErrorCode code, string message) =>
            new(null, null, DedicatedSaveResult.Fail(code, message));
    }

    private sealed class CopyVerificationException(string message) : IOException(message);
}
