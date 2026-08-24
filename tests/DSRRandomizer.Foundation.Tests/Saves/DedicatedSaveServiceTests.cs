using System.Security.Cryptography;
using System.Runtime.InteropServices;
using System.Text.Json;
using DSRRandomizer.Foundation.Paths;
using DSRRandomizer.Foundation.Saves;

namespace DSRRandomizer.Foundation.Tests.Saves;

public sealed class DedicatedSaveServiceTests : IDisposable
{
    private const long FixedSaveLength = 4_326_608;
    private readonly string _root = Path.Combine(
        Path.GetTempPath(),
        "DSRRandomizer.Tests",
        Guid.NewGuid().ToString("N"));

    [Fact]
    public async Task PrepareAsync_ValidExistingRmm_NeverOpensNormalSave()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateValidExternalRmm();

        var result = await fixture.Service.PrepareAsync(fixture.SteamId, default);

        Assert.True(result.Ready, result.Message);
        Assert.True(result.ReusedExisting);
        Assert.DoesNotContain(
            fixture.Access.Opens,
            open => open.Path.EndsWith(".sl2", StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public async Task PrepareAsync_HardLinkedRmm_IsRejectedWithoutChangingNormalSave()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateNormalSave();
        fixture.CreateHardLinkedExternalRmmToNormal();
        var before = fixture.CaptureSourceState();

        var result = await fixture.Service.PrepareAsync(fixture.SteamId, default);

        Assert.False(result.Ready);
        Assert.Equal(SaveErrorCode.ExistingSaveInvalid, result.ErrorCode);
        Assert.Equal(before, fixture.CaptureSourceState());
    }

    [Fact]
    public async Task PrepareAsync_RmmBecomesHardLinkAfterInspection_IsRejectedWithoutChangingNormalSave()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateNormalSave(fill: 0x52);
        fixture.CreateValidExternalRmm(fill: 0x52);
        var before = fixture.CaptureSourceState();
        var swapped = false;
        fixture.Access.AfterSingleLinkCheck = (path, isSingleLink) =>
        {
            if (swapped
                || !isSingleLink
                || !path.Equals(fixture.DedicatedPath, StringComparison.OrdinalIgnoreCase))
            {
                return;
            }

            swapped = true;
            File.Delete(fixture.DedicatedPath);
            if (!CreateHardLinkW(fixture.DedicatedPath, fixture.SourcePath, IntPtr.Zero))
            {
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            }
        };

        var result = await fixture.Service.PrepareAsync(fixture.SteamId, default);

        Assert.True(swapped);
        Assert.False(result.Ready);
        Assert.Equal(SaveErrorCode.ExistingSaveInvalid, result.ErrorCode);
        Assert.Equal(before, fixture.CaptureSourceState());
    }

    [Fact]
    public async Task PrepareAsync_FirstBootstrap_OpensExactSelectedSourceReadOnlyWithoutDeleteSharing()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateNormalSave();
        var before = fixture.CaptureSourceState();

        var result = await fixture.Service.PrepareAsync(fixture.SteamId, default);

        Assert.True(result.Ready);
        Assert.False(result.ReusedExisting);
        var sourceOpen = Assert.Single(
            fixture.Access.Opens,
            open => open.Path.Equals(fixture.SourcePath, StringComparison.OrdinalIgnoreCase));
        Assert.Equal(FileMode.Open, sourceOpen.Mode);
        Assert.Equal(FileAccess.Read, sourceOpen.Access);
        Assert.Equal(FileShare.Read, sourceOpen.Share);
        Assert.Equal(before, fixture.CaptureSourceState());
        Assert.Equal(fixture.NormalBytes, File.ReadAllBytes(fixture.DedicatedPath));
    }

    [Fact]
    public async Task PrepareAsync_SourceHasWrongLength_FailsClosedAndPreservesSource()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateNormalSave(length: 128);
        var before = fixture.CaptureSourceState();

        var result = await fixture.Service.PrepareAsync(fixture.SteamId, default);

        Assert.False(result.Ready);
        Assert.Equal(SaveErrorCode.CopyVerificationFailed, result.ErrorCode);
        Assert.Equal(before, fixture.CaptureSourceState());
        Assert.False(File.Exists(fixture.DedicatedPath));
    }

    [Fact]
    public async Task PrepareAsync_SourceChangesDuringCopy_FailsClosedWithoutPublishingDestination()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateNormalSave();
        fixture.Access.AfterCopy = _ => File.SetLastWriteTimeUtc(
            fixture.SourcePath,
            DateTime.UtcNow.AddMinutes(1));

        var result = await fixture.Service.PrepareAsync(fixture.SteamId, default);

        Assert.False(result.Ready);
        Assert.Equal(SaveErrorCode.SourceChanged, result.ErrorCode);
        Assert.False(File.Exists(fixture.DedicatedPath));
        Assert.Equal(fixture.NormalBytes, File.ReadAllBytes(fixture.SourcePath));
    }

    [Fact]
    public async Task PrepareAsync_ShortCopy_FailsVerificationAndDeletesOnlyStaging()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateNormalSave();
        fixture.Access.CopyBehavior = CopyBehavior.Short;
        var before = fixture.CaptureSourceState();

        var result = await fixture.Service.PrepareAsync(fixture.SteamId, default);

        Assert.False(result.Ready);
        Assert.Equal(SaveErrorCode.CopyVerificationFailed, result.ErrorCode);
        Assert.Equal(before, fixture.CaptureSourceState());
        Assert.False(File.Exists(fixture.DedicatedPath));
        Assert.Empty(Directory.EnumerateFiles(fixture.Layout.Staging));
    }

    [Fact]
    public async Task PrepareAsync_StagedHashMismatch_FailsVerificationAndPreservesSource()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateNormalSave();
        fixture.Access.CopyBehavior = CopyBehavior.CorruptSameLength;
        var before = fixture.CaptureSourceState();

        var result = await fixture.Service.PrepareAsync(fixture.SteamId, default);

        Assert.False(result.Ready);
        Assert.Equal(SaveErrorCode.CopyVerificationFailed, result.ErrorCode);
        Assert.Equal(before, fixture.CaptureSourceState());
        Assert.False(File.Exists(fixture.DedicatedPath));
    }

    [Fact]
    public async Task PrepareAsync_StageIsReplacedBeforeVerification_PreservesForeignWinner()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateNormalSave();
        var foreignBytes = new byte[] { 0x46, 0x4F, 0x52, 0x45, 0x49, 0x47, 0x4E };
        string? replacedStagePath = null;
        fixture.Access.AfterCopy = path =>
        {
            replacedStagePath = path;
            File.Delete(path);
            File.WriteAllBytes(path, foreignBytes);
        };

        var result = await fixture.Service.PrepareAsync(fixture.SteamId, default);

        Assert.False(result.Ready);
        var stagePath = Assert.IsType<string>(replacedStagePath);
        Assert.True(File.Exists(stagePath));
        Assert.Equal(foreignBytes, File.ReadAllBytes(stagePath));
        Assert.False(File.Exists(fixture.DedicatedPath));
    }

    [Fact]
    public async Task PrepareAsync_DestinationAppearsAtPublishTime_DoesNotOverwriteRaceWinner()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateNormalSave();
        var raceBytes = Enumerable.Repeat((byte)0x7A, checked((int)FixedSaveLength)).ToArray();
        fixture.Access.RaceDestinationPath = fixture.DedicatedPath;
        fixture.Access.RaceDestinationBytes = raceBytes;

        var result = await fixture.Service.PrepareAsync(fixture.SteamId, default);

        Assert.False(result.Ready);
        Assert.Equal(SaveErrorCode.DestinationRace, result.ErrorCode);
        Assert.Equal(raceBytes, File.ReadAllBytes(fixture.DedicatedPath));
        Assert.Empty(Directory.EnumerateFiles(fixture.Layout.Staging));
    }

    [Fact]
    public async Task PrepareAsync_MetadataWriteFailsAfterCreatingTemp_RemovesOwnedSaveMetadataAndTemp()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateNormalSave();
        fixture.Access.FailMetadataWriteAfterCreate = true;

        var result = await fixture.Service.PrepareAsync(fixture.SteamId, default);

        Assert.False(result.Ready);
        Assert.False(File.Exists(fixture.DedicatedPath));
        Assert.False(File.Exists(fixture.MetadataPath));
        Assert.Empty(Directory.EnumerateFiles(fixture.SaveDirectory));
    }

    [Fact]
    public async Task PrepareAsync_MetadataTempIsReplacedBeforeWriteFailure_PreservesForeignWinner()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateNormalSave();
        var foreignBytes = new byte[] { 0x46, 0x4F, 0x52, 0x45, 0x49, 0x47, 0x4E };
        fixture.Access.ReplaceMetadataTemporaryBeforeWriteFailure = foreignBytes;

        var result = await fixture.Service.PrepareAsync(fixture.SteamId, default);

        Assert.False(result.Ready);
        Assert.False(File.Exists(fixture.DedicatedPath));
        var foreignPath = Assert.IsType<string>(fixture.Access.ReplacedMetadataTemporaryPath);
        Assert.True(File.Exists(foreignPath));
        Assert.Equal(foreignBytes, File.ReadAllBytes(foreignPath));
    }

    [Fact]
    public async Task PrepareAsync_MetadataReplaceFails_RemovesOwnedSaveMetadataAndTemp()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateNormalSave();
        fixture.Access.FailNextMetadataReplace = true;

        var result = await fixture.Service.PrepareAsync(fixture.SteamId, default);

        Assert.False(result.Ready);
        Assert.False(File.Exists(fixture.DedicatedPath));
        Assert.False(File.Exists(fixture.MetadataPath));
        Assert.Empty(Directory.EnumerateFiles(fixture.SaveDirectory));
    }

    [Fact]
    public async Task PrepareAsync_FinalVerificationFails_RemovesOwnedSaveAndMetadata()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateNormalSave();
        fixture.Access.CorruptOnHashPath = fixture.DedicatedPath;
        fixture.Access.CorruptOnHashPathCall = 1;

        var result = await fixture.Service.PrepareAsync(fixture.SteamId, default);

        Assert.False(result.Ready);
        Assert.False(File.Exists(fixture.DedicatedPath));
        Assert.False(File.Exists(fixture.MetadataPath));
    }

    [Fact]
    public async Task PrepareAsync_CancellationAfterSavePublication_RollsBackOwnedArtifactsAndRethrows()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateNormalSave();
        using var cancellation = new CancellationTokenSource();
        fixture.Access.AfterMoveCreateNew = (_, destination) =>
        {
            if (destination.Equals(fixture.DedicatedPath, StringComparison.OrdinalIgnoreCase))
            {
                cancellation.Cancel();
            }
        };

        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => fixture.Service.PrepareAsync(fixture.SteamId, cancellation.Token));

        Assert.False(File.Exists(fixture.DedicatedPath));
        Assert.False(File.Exists(fixture.MetadataPath));
        Assert.Empty(Directory.EnumerateFiles(fixture.SaveDirectory));
    }

    [Fact]
    public async Task PrepareAsync_MoveReportsFailureAfterPublication_RemovesExactOwnedSave()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateNormalSave();
        fixture.Access.AfterMoveCreateNew = (_, destination) =>
        {
            if (destination.Equals(fixture.DedicatedPath, StringComparison.OrdinalIgnoreCase))
            {
                throw new IOException("Injected post-publication move failure.");
            }
        };

        var result = await fixture.Service.PrepareAsync(fixture.SteamId, default);

        Assert.False(result.Ready);
        Assert.False(File.Exists(fixture.DedicatedPath));
        Assert.False(File.Exists(fixture.MetadataPath));
    }

    [Fact]
    public async Task PrepareAsync_InvalidExistingMetadata_FailsWithoutOpeningNormalSaveOrChangingDestination()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateNormalSave();
        Directory.CreateDirectory(fixture.SaveDirectory);
        File.WriteAllBytes(fixture.DedicatedPath, fixture.NormalBytes);
        File.WriteAllText(fixture.MetadataPath, "{not-json");
        var before = File.ReadAllBytes(fixture.DedicatedPath);

        var result = await fixture.Service.PrepareAsync(fixture.SteamId, default);

        Assert.False(result.Ready);
        Assert.Equal(SaveErrorCode.ExistingSaveInvalid, result.ErrorCode);
        Assert.DoesNotContain(
            fixture.Access.Opens,
            open => open.Path.EndsWith(".sl2", StringComparison.OrdinalIgnoreCase));
        Assert.Equal(before, File.ReadAllBytes(fixture.DedicatedPath));
    }

    [Fact]
    public async Task PrepareAsync_NullSteamIdMetadata_BlocksWithoutOpeningNormalSave()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateValidExternalRmm();
        File.WriteAllText(
            fixture.MetadataPath,
            $$"""
            {"schemaVersion":1,"steamId":null,"fixedLength":{{FixedSaveLength}},"lastKnownSha256":"{{new string('0', 64)}}","activeSeedId":null,"placementSha256":null,"cleanExit":false}
            """);

        var result = await fixture.Service.PrepareAsync(fixture.SteamId, default);

        Assert.False(result.Ready);
        Assert.Equal(SaveErrorCode.ExistingSaveInvalid, result.ErrorCode);
        Assert.DoesNotContain(
            fixture.Access.Opens,
            open => open.Path.EndsWith(".sl2", StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public async Task PrepareAsync_MetadataPlacementHashIsNotHex_BlocksWithoutOpeningNormalSave()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateValidExternalRmm(
            seed: new SeedBinding("seed", new string('z', 64)));

        var result = await fixture.Service.PrepareAsync(fixture.SteamId, default);

        Assert.False(result.Ready);
        Assert.Equal(SaveErrorCode.ExistingSaveInvalid, result.ErrorCode);
        Assert.DoesNotContain(
            fixture.Access.Opens,
            open => open.Path.EndsWith(".sl2", StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public async Task PrepareAsync_CleanExistingHashMismatch_FailsWithoutOpeningNormalSave()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateNormalSave();
        fixture.CreateValidExternalRmm(lastKnownSha256: new string('0', 64));

        var result = await fixture.Service.PrepareAsync(fixture.SteamId, default);

        Assert.False(result.Ready);
        Assert.Equal(SaveErrorCode.ExistingSaveInvalid, result.ErrorCode);
        Assert.DoesNotContain(
            fixture.Access.Opens,
            open => open.Path.EndsWith(".sl2", StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public async Task PrepareAsync_UncleanExistingWithNonHexMetadataHash_FailsAsInvalidMetadata()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateValidExternalRmm(
            lastKnownSha256: new string('g', 64),
            cleanExit: false);

        var result = await fixture.Service.PrepareAsync(fixture.SteamId, default);

        Assert.False(result.Ready);
        Assert.Equal(SaveErrorCode.ExistingSaveInvalid, result.ErrorCode);
    }

    [Fact]
    public async Task PrepareAsync_ExistingWrongLength_FailsClosedAndPreservesDestination()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateValidExternalRmm(length: 64);
        var before = File.ReadAllBytes(fixture.DedicatedPath);

        var result = await fixture.Service.PrepareAsync(fixture.SteamId, default);

        Assert.False(result.Ready);
        Assert.Equal(SaveErrorCode.ExistingSaveInvalid, result.ErrorCode);
        Assert.Equal(before, File.ReadAllBytes(fixture.DedicatedPath));
    }

    [Fact]
    public async Task PrepareAsync_ReparseEscape_FailsBeforeWritingOutsideBoundary()
    {
        var canonicalizer = new EscapingCanonicalizer();
        var fixture = await Fixture.CreateAsync(_root, canonicalizer);
        fixture.CreateNormalSave();
        canonicalizer.EscapePrefix = fixture.SaveDirectory;
        canonicalizer.EscapeTarget = Path.Combine(_root, "outside");

        var result = await fixture.Service.PrepareAsync(fixture.SteamId, default);

        Assert.False(result.Ready);
        Assert.Equal(SaveErrorCode.PathDenied, result.ErrorCode);
        Assert.False(Directory.Exists(canonicalizer.EscapeTarget));
        Assert.False(File.Exists(fixture.DedicatedPath));
    }

    [Fact]
    public async Task PrepareAsync_ParentIsSwappedAfterBoundaryValidation_LeavesOutsideTargetUntouched()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateNormalSave();
        var outside = Path.Combine(_root, "outside");
        var displaced = Path.Combine(_root, "displaced-save-directory");
        Directory.CreateDirectory(outside);
        fixture.Access.AfterMutationLeaseAcquired = path =>
        {
            if (!path.Equals(fixture.SaveDirectory, StringComparison.OrdinalIgnoreCase))
            {
                return;
            }

            fixture.Access.ParentSwapAttempted = true;
            try
            {
                Directory.Move(path, displaced);
                Directory.CreateDirectory(path);
                fixture.Access.RedirectFrom = path;
                fixture.Access.RedirectTo = outside;
            }
            catch (IOException)
            {
                // A no-delete directory lease must make this interleaving fail.
            }
        };

        await fixture.Service.PrepareAsync(fixture.SteamId, default);

        Assert.True(fixture.Access.ParentSwapAttempted);
        Assert.False(fixture.Access.RedirectedMutationObserved);
        Assert.Empty(Directory.EnumerateFileSystemEntries(outside));
    }

    [Fact]
    public async Task ResetForSeedAsync_PersistentSaveArchiveFailure_PreservesLivePair()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateNormalSave(fill: 0x41);
        fixture.CreateValidExternalRmm(fill: 0x22, seed: Fixture.OldSeed);
        var priorSave = File.ReadAllBytes(fixture.DedicatedPath);
        var priorMetadata = File.ReadAllBytes(fixture.MetadataPath);
        fixture.Access.FailSaveArchiveMoves = true;

        var result = await fixture.Service.ResetForSeedAsync(
            fixture.SteamId,
            Fixture.NewSeed,
            default);

        Assert.False(result.Ready);
        Assert.Equal(priorSave, File.ReadAllBytes(fixture.DedicatedPath));
        Assert.Equal(priorMetadata, File.ReadAllBytes(fixture.MetadataPath));
        var archive = Path.Combine(fixture.SaveDirectory, "archive");
        Assert.Empty(Directory.EnumerateFiles(archive));
    }

    [Fact]
    public async Task ResetForSeedAsync_MetadataPublishFailure_RestoresPreviousSaveAndMetadata()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateNormalSave(fill: 0x41);
        fixture.CreateValidExternalRmm(fill: 0x22, seed: Fixture.OldSeed);
        var priorSave = File.ReadAllBytes(fixture.DedicatedPath);
        var priorMetadata = File.ReadAllBytes(fixture.MetadataPath);
        fixture.Access.FailNextMetadataReplace = true;

        var result = await fixture.Service.ResetForSeedAsync(
            fixture.SteamId,
            Fixture.NewSeed,
            default);

        Assert.False(result.Ready);
        Assert.Equal(priorSave, File.ReadAllBytes(fixture.DedicatedPath));
        Assert.Equal(priorMetadata, File.ReadAllBytes(fixture.MetadataPath));
        Assert.Equal(fixture.NormalBytes, File.ReadAllBytes(fixture.SourcePath));
        Assert.Empty(Directory.EnumerateFiles(fixture.Layout.Staging));
    }

    [Fact]
    public async Task ResetForSeedAsync_SuccessPublishesSourceCopyAndNewBinding()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateNormalSave(fill: 0x41);
        fixture.CreateValidExternalRmm(fill: 0x22, seed: Fixture.OldSeed);

        var result = await fixture.Service.ResetForSeedAsync(
            fixture.SteamId,
            Fixture.NewSeed,
            default);

        Assert.True(result.Ready);
        Assert.Equal(fixture.NormalBytes, File.ReadAllBytes(fixture.DedicatedPath));
        var metadata = fixture.ReadMetadata();
        Assert.Equal("new-seed", metadata.ActiveSeedId);
        Assert.Equal(Fixture.NewSeed.PlacementSha256, metadata.PlacementSha256);
        Assert.True(metadata.CleanExit);
        Assert.Contains(
            Directory.EnumerateFiles(Path.Combine(fixture.SaveDirectory, "archive")),
            path => path.EndsWith(".rmm", StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public async Task ResetForSeedAsync_FinalVerificationFailure_RestoresPreviousSaveAndMetadata()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateNormalSave(fill: 0x41);
        fixture.CreateValidExternalRmm(fill: 0x22, seed: Fixture.OldSeed);
        var priorSave = File.ReadAllBytes(fixture.DedicatedPath);
        var priorMetadata = File.ReadAllBytes(fixture.MetadataPath);
        fixture.Access.CorruptOnHashPath = fixture.DedicatedPath;
        fixture.Access.CorruptOnHashPathCall = 3;

        var result = await fixture.Service.ResetForSeedAsync(
            fixture.SteamId,
            Fixture.NewSeed,
            default);

        Assert.False(result.Ready);
        Assert.Equal(priorSave, File.ReadAllBytes(fixture.DedicatedPath));
        Assert.Equal(priorMetadata, File.ReadAllBytes(fixture.MetadataPath));
    }

    [Theory]
    [InlineData("placement")]
    [InlineData("zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz")]
    public async Task ResetForSeedAsync_InvalidPlacementHash_IsRejectedBeforeSourceOpen(string placementHash)
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateNormalSave();
        fixture.CreateValidExternalRmm(seed: Fixture.OldSeed);

        var result = await fixture.Service.ResetForSeedAsync(
            fixture.SteamId,
            new SeedBinding("new-seed", placementHash),
            default);

        Assert.False(result.Ready);
        Assert.Equal(SaveErrorCode.SeedMismatch, result.ErrorCode);
        Assert.DoesNotContain(
            fixture.Access.Opens,
            open => open.Path.EndsWith(".sl2", StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public async Task ResetForSeedAsync_CancellationAfterArchive_RestoresPreviousPairAndRethrows()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateNormalSave(fill: 0x41);
        fixture.CreateValidExternalRmm(fill: 0x22, seed: Fixture.OldSeed);
        var priorSave = File.ReadAllBytes(fixture.DedicatedPath);
        var priorMetadata = File.ReadAllBytes(fixture.MetadataPath);
        using var cancellation = new CancellationTokenSource();
        fixture.Access.AfterMoveCreateNew = (source, destination) =>
        {
            if (source.Equals(fixture.DedicatedPath, StringComparison.OrdinalIgnoreCase)
                && destination.EndsWith(".rmm", StringComparison.OrdinalIgnoreCase))
            {
                cancellation.Cancel();
            }
        };

        await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            fixture.Service.ResetForSeedAsync(
                fixture.SteamId,
                Fixture.NewSeed,
                cancellation.Token));

        Assert.True(
            File.Exists(fixture.DedicatedPath),
            string.Join(Environment.NewLine, Directory.EnumerateFiles(_root, "*", SearchOption.AllDirectories)));
        Assert.Equal(priorSave, File.ReadAllBytes(fixture.DedicatedPath));
        Assert.Equal(priorMetadata, File.ReadAllBytes(fixture.MetadataPath));
    }

    [Fact]
    public async Task ResetForSeedAsync_CancellationAfterNewSavePublication_RestoresPreviousPairAndRethrows()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateNormalSave(fill: 0x41);
        fixture.CreateValidExternalRmm(fill: 0x22, seed: Fixture.OldSeed);
        var priorSave = File.ReadAllBytes(fixture.DedicatedPath);
        var priorMetadata = File.ReadAllBytes(fixture.MetadataPath);
        using var cancellation = new CancellationTokenSource();
        var published = false;
        fixture.Access.AfterMoveCreateNew = (_, destination) =>
        {
            if (!published
                && destination.Equals(fixture.DedicatedPath, StringComparison.OrdinalIgnoreCase))
            {
                published = true;
                cancellation.Cancel();
            }
        };

        await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            fixture.Service.ResetForSeedAsync(
                fixture.SteamId,
                Fixture.NewSeed,
                cancellation.Token));

        Assert.Equal(priorSave, File.ReadAllBytes(fixture.DedicatedPath));
        Assert.Equal(priorMetadata, File.ReadAllBytes(fixture.MetadataPath));
    }

    [Fact]
    public async Task ResetForSeedAsync_CancellationDuringPreMutationSnapshot_RemovesStagingAndPreservesPreviousPair()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateNormalSave(fill: 0x41);
        fixture.CreateValidExternalRmm(fill: 0x22, seed: Fixture.OldSeed);
        var priorSave = File.ReadAllBytes(fixture.DedicatedPath);
        var priorMetadata = File.ReadAllBytes(fixture.MetadataPath);
        using var cancellation = new CancellationTokenSource();
        fixture.Access.BeforePathIdentityAndHash = (path, call) =>
        {
            if (call == 2 && path.Equals(fixture.DedicatedPath, StringComparison.OrdinalIgnoreCase))
            {
                cancellation.Cancel();
            }
        };

        await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            fixture.Service.ResetForSeedAsync(
                fixture.SteamId,
                Fixture.NewSeed,
                cancellation.Token));

        Assert.Empty(Directory.EnumerateFiles(fixture.Layout.Staging));
        Assert.Equal(priorSave, File.ReadAllBytes(fixture.DedicatedPath));
        Assert.Equal(priorMetadata, File.ReadAllBytes(fixture.MetadataPath));
    }

    [Fact]
    public async Task ResetForSeedAsync_NewSaveIsReplacedBeforeRollback_PreservesForeignWinnerAndArchivesOldPair()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateNormalSave(fill: 0x41);
        fixture.CreateValidExternalRmm(fill: 0x22, seed: Fixture.OldSeed);
        var priorSave = File.ReadAllBytes(fixture.DedicatedPath);
        var priorMetadata = File.ReadAllBytes(fixture.MetadataPath);
        var foreign = fixture.Bytes(0x7B);
        fixture.Access.CorruptOnHashPath = fixture.DedicatedPath;
        fixture.Access.CorruptOnHashPathCall = 3;
        var replaced = false;
        fixture.Access.BeforeOwnedDelete = path =>
        {
            if (!replaced && path.Equals(fixture.DedicatedPath, StringComparison.OrdinalIgnoreCase))
            {
                replaced = true;
                File.Delete(path);
                File.WriteAllBytes(path, foreign);
            }
        };

        var result = await fixture.Service.ResetForSeedAsync(
            fixture.SteamId,
            Fixture.NewSeed,
            default);

        Assert.False(result.Ready);
        Assert.Equal(foreign, File.ReadAllBytes(fixture.DedicatedPath));
        Assert.False(File.Exists(fixture.MetadataPath));
        fixture.AssertArchiveContains(priorSave, priorMetadata);
    }

    [Fact]
    public async Task ResetForSeedAsync_RaceBlocksOldSaveRestore_RearchivesOldMetadataBesideOldSave()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateNormalSave(fill: 0x41);
        fixture.CreateValidExternalRmm(fill: 0x22, seed: Fixture.OldSeed);
        var priorSave = File.ReadAllBytes(fixture.DedicatedPath);
        var priorMetadata = File.ReadAllBytes(fixture.MetadataPath);
        var foreign = fixture.Bytes(0x7C);
        fixture.Access.CorruptOnHashPath = fixture.DedicatedPath;
        fixture.Access.CorruptOnHashPathCall = 3;
        var raced = false;
        fixture.Access.BeforeMoveCreateNew = (source, destination) =>
        {
            if (!raced
                && source.Contains("archive", StringComparison.OrdinalIgnoreCase)
                && source.EndsWith(".rmm", StringComparison.OrdinalIgnoreCase)
                && destination.Equals(fixture.DedicatedPath, StringComparison.OrdinalIgnoreCase))
            {
                raced = true;
                File.WriteAllBytes(destination, foreign);
            }
        };

        var result = await fixture.Service.ResetForSeedAsync(
            fixture.SteamId,
            Fixture.NewSeed,
            default);

        Assert.False(result.Ready);
        Assert.Equal(foreign, File.ReadAllBytes(fixture.DedicatedPath));
        Assert.False(File.Exists(fixture.MetadataPath));
        fixture.AssertArchiveContains(priorSave, priorMetadata);
    }

    [Fact]
    public async Task BeginSessionAsync_PersistsUncleanStateBeforeReturningReady()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateValidExternalRmm();

        var result = await fixture.Service.BeginSessionAsync(fixture.SteamId, default);

        Assert.True(result.Ready, result.Message);
        Assert.False(string.IsNullOrWhiteSpace(result.SessionToken));
        Assert.False(fixture.ReadMetadata().CleanExit);
        Assert.True(fixture.Access.MetadataReplaceCompleted);

        await fixture.Service.CompleteSessionAsync(
            fixture.SteamId,
            result.SessionToken!,
            normalGuardedExit: false,
            default);
    }

    [Fact]
    public async Task CompleteSessionAsync_AbnormalExit_DoesNotUpdateHashOrMarkClean()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateValidExternalRmm();
        var session = await fixture.Service.BeginSessionAsync(fixture.SteamId, default);
        File.WriteAllBytes(fixture.DedicatedPath, fixture.Bytes(0x66));
        var before = fixture.ReadMetadata();

        var result = await fixture.Service.CompleteSessionAsync(
            fixture.SteamId,
            session.SessionToken!,
            normalGuardedExit: false,
            default);

        Assert.False(result.Ready);
        Assert.Equal(before, fixture.ReadMetadata());
        Assert.False(fixture.ReadMetadata().CleanExit);
    }

    [Fact]
    public async Task CompleteSessionAsync_NormalGuardedExit_WritesNewHashAndMarksClean()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateValidExternalRmm();
        var session = await fixture.Service.BeginSessionAsync(fixture.SteamId, default);
        var changed = fixture.Bytes(0x66);
        File.WriteAllBytes(fixture.DedicatedPath, changed);

        var result = await fixture.Service.CompleteSessionAsync(
            fixture.SteamId,
            session.SessionToken!,
            normalGuardedExit: true,
            default);

        Assert.True(result.Ready);
        var metadata = fixture.ReadMetadata();
        Assert.True(metadata.CleanExit);
        Assert.Equal(Convert.ToHexString(SHA256.HashData(changed)).ToLowerInvariant(), metadata.LastKnownSha256);
    }

    [Fact]
    public async Task CompleteSessionAsync_BeforeBegin_DoesNotMarkMetadataCleanAgain()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateValidExternalRmm();
        var before = fixture.ReadMetadata();

        var result = await fixture.Service.CompleteSessionAsync(
            fixture.SteamId,
            Guid.NewGuid().ToString("N"),
            normalGuardedExit: true,
            default);

        Assert.False(result.Ready);
        Assert.Equal(before, fixture.ReadMetadata());
    }

    [Fact]
    public async Task BeginSessionAsync_OverlappingBeginIsRejectedAndCannotCompleteFirstLease()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateValidExternalRmm();
        var first = await fixture.Service.BeginSessionAsync(fixture.SteamId, default);
        var second = await fixture.Service.BeginSessionAsync(fixture.SteamId, default);

        Assert.True(first.Ready);
        Assert.False(second.Ready);
        Assert.Equal(SaveErrorCode.SessionAlreadyActive, second.ErrorCode);
        Assert.Null(second.SessionToken);

        var rejectedCompletion = await fixture.Service.CompleteSessionAsync(
            fixture.SteamId,
            Guid.NewGuid().ToString("N"),
            normalGuardedExit: true,
            default);

        Assert.False(rejectedCompletion.Ready);
        Assert.False(fixture.ReadMetadata().CleanExit);

        var current = await fixture.Service.CompleteSessionAsync(
            fixture.SteamId,
            first.SessionToken!,
            normalGuardedExit: true,
            default);
        Assert.True(current.Ready);
        Assert.True(fixture.ReadMetadata().CleanExit);
    }

    [Fact]
    public async Task BeginSessionAsync_SameProfileAcrossServiceInstances_IsRejectedAsActive()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateValidExternalRmm();
        var firstService = fixture.CreateService();
        var secondService = fixture.CreateService();

        var first = await firstService.BeginSessionAsync(fixture.SteamId, default);
        var second = await secondService.BeginSessionAsync(fixture.SteamId, default);

        Assert.True(first.Ready, first.Message);
        Assert.False(second.Ready);
        Assert.Equal(SaveErrorCode.SessionAlreadyActive, second.ErrorCode);

        var completed = await firstService.CompleteSessionAsync(
            fixture.SteamId,
            first.SessionToken!,
            normalGuardedExit: true,
            default);
        Assert.True(completed.Ready, completed.Message);
    }

    [Fact]
    public async Task BeginSessionAsync_DifferentProfiles_AreNotGloballySerialized()
    {
        const string secondSteamId = "22345678901234567";
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateValidExternalRmm();
        fixture.CreateValidExternalRmm(secondSteamId, fill: 0x53);
        var firstService = fixture.CreateService();
        var secondService = fixture.CreateService();

        var first = await firstService.BeginSessionAsync(fixture.SteamId, default);
        var second = await secondService.BeginSessionAsync(secondSteamId, default);

        Assert.True(first.Ready, first.Message);
        Assert.True(second.Ready, second.Message);

        var firstCompleted = await firstService.CompleteSessionAsync(
            fixture.SteamId,
            first.SessionToken!,
            normalGuardedExit: true,
            default);
        var secondCompleted = await secondService.CompleteSessionAsync(
            secondSteamId,
            second.SessionToken!,
            normalGuardedExit: true,
            default);
        Assert.True(firstCompleted.Ready, firstCompleted.Message);
        Assert.True(secondCompleted.Ready, secondCompleted.Message);
    }

    [Fact]
    public async Task BeginSessionAsync_HardLinkedSessionLock_IsRejected()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateValidExternalRmm();
        var lockTarget = Path.Combine(_root, "lock-target.bin");
        File.WriteAllBytes(lockTarget, []);
        if (!CreateHardLinkW(
                Path.Combine(fixture.SaveDirectory, ".session.lock"),
                lockTarget,
                IntPtr.Zero))
        {
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        }

        var result = await fixture.Service.BeginSessionAsync(fixture.SteamId, default);

        Assert.False(result.Ready);
        Assert.Equal(SaveErrorCode.PathDenied, result.ErrorCode);
    }

    [Fact]
    public async Task CompleteSessionAsync_ConcurrentSecondCompletion_IsRejectedBeforeLockRelease()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateValidExternalRmm();
        var session = await fixture.Service.BeginSessionAsync(fixture.SteamId, default);
        using var enteredHash = new ManualResetEventSlim();
        using var releaseHash = new ManualResetEventSlim();
        var blocked = 0;
        fixture.Access.BeforePathIdentityAndHash = (path, _) =>
        {
            if (path.Equals(fixture.DedicatedPath, StringComparison.OrdinalIgnoreCase)
                && Interlocked.CompareExchange(ref blocked, 1, 0) == 0)
            {
                enteredHash.Set();
                releaseHash.Wait(TimeSpan.FromSeconds(10));
            }
        };

        var firstCompletion = Task.Run(() => fixture.Service.CompleteSessionAsync(
            fixture.SteamId,
            session.SessionToken!,
            normalGuardedExit: true,
            default));
        Assert.True(enteredHash.Wait(TimeSpan.FromSeconds(10)));

        DedicatedSaveResult second;
        try
        {
            second = await fixture.Service.CompleteSessionAsync(
                fixture.SteamId,
                session.SessionToken!,
                normalGuardedExit: true,
                default);
        }
        finally
        {
            releaseHash.Set();
        }

        var first = await firstCompletion;
        Assert.False(second.Ready);
        Assert.True(first.Ready, first.Message);
    }

    public void Dispose()
    {
        if (Directory.Exists(_root))
        {
            foreach (var file in Directory.EnumerateFiles(_root, "*", SearchOption.AllDirectories))
            {
                File.SetAttributes(file, FileAttributes.Normal);
            }

            Directory.Delete(_root, recursive: true);
        }
    }

    private sealed class Fixture
    {
        public static readonly SeedBinding OldSeed =
            new("old-seed", new string('1', 64));
        public static readonly SeedBinding NewSeed =
            new("new-seed", new string('2', 64));
        private static readonly JsonSerializerOptions JsonOptions = new()
        {
            PropertyNamingPolicy = JsonNamingPolicy.CamelCase
        };

        private Fixture(
            string root,
            LocalDataLayout layout,
            WriteBoundary boundary,
            RecordingFileAccess access)
        {
            SteamId = "12345678901234567";
            Layout = layout;
            Boundary = boundary;
            Access = access;
            SourcePath = Path.Combine(root, "documents", SteamId, "DRAKS0005.sl2");
            SaveDirectory = Path.Combine(layout.Saves, SteamId);
            DedicatedPath = Path.Combine(SaveDirectory, "DRAKS0005.rmm");
            MetadataPath = Path.Combine(SaveDirectory, "save-metadata.json");
            var selectionStore = new SaveSelectionStore(layout, boundary);
            Service = new DedicatedSaveService(layout, boundary, selectionStore, access);
        }

        public string SteamId { get; }
        public string SourcePath { get; }
        public string SaveDirectory { get; }
        public string DedicatedPath { get; }
        public string MetadataPath { get; }
        public LocalDataLayout Layout { get; }
        public WriteBoundary Boundary { get; }
        public RecordingFileAccess Access { get; }
        public DedicatedSaveService Service { get; }
        public byte[] NormalBytes { get; private set; } = [];

        public static async Task<Fixture> CreateAsync(
            string root,
            IPathCanonicalizer? canonicalizer = null)
        {
            Directory.CreateDirectory(root);
            var sourceInstallation = Path.Combine(root, "stock-game");
            var local = Path.Combine(root, "local");
            Directory.CreateDirectory(sourceInstallation);
            Directory.CreateDirectory(local);
            canonicalizer ??= new WindowsPathCanonicalizer();
            var boundary = WriteBoundary.Create(sourceInstallation, local, canonicalizer);
            var layout = LocalDataLayout.Create(local, boundary);
            Directory.CreateDirectory(layout.Config);
            Directory.CreateDirectory(layout.Staging);
            var access = new RecordingFileAccess(new SystemFileAccess());
            var fixture = new Fixture(root, layout, boundary, access);
            Directory.CreateDirectory(Path.GetDirectoryName(fixture.SourcePath)!);
            var store = new SaveSelectionStore(layout, boundary);
            await store.WriteAsync(
                new SaveProfileCandidate(fixture.SteamId, fixture.SourcePath),
                default);
            return fixture;
        }

        public byte[] Bytes(byte fill, int length = checked((int)FixedSaveLength)) =>
            Enumerable.Repeat(fill, length).ToArray();

        public void CreateNormalSave(byte fill = 0x31, int length = checked((int)FixedSaveLength))
        {
            NormalBytes = Bytes(fill, length);
            File.WriteAllBytes(SourcePath, NormalBytes);
            File.SetAttributes(SourcePath, FileAttributes.ReadOnly | FileAttributes.Archive);
        }

        public void CreateValidExternalRmm(
            byte fill = 0x52,
            int length = checked((int)FixedSaveLength),
            string? lastKnownSha256 = null,
            SeedBinding? seed = null,
            bool cleanExit = true)
            => CreateValidExternalRmm(
                SteamId,
                fill,
                length,
                lastKnownSha256,
                seed,
                cleanExit);

        public void CreateValidExternalRmm(
            string steamId,
            byte fill = 0x52,
            int length = checked((int)FixedSaveLength),
            string? lastKnownSha256 = null,
            SeedBinding? seed = null,
            bool cleanExit = true)
        {
            var saveDirectory = Path.Combine(Layout.Saves, steamId);
            var dedicatedPath = Path.Combine(saveDirectory, "DRAKS0005.rmm");
            var metadataPath = Path.Combine(saveDirectory, "save-metadata.json");
            Directory.CreateDirectory(saveDirectory);
            var bytes = Bytes(fill, length);
            File.WriteAllBytes(dedicatedPath, bytes);
            File.WriteAllBytes(metadataPath, JsonSerializer.SerializeToUtf8Bytes(
                new DedicatedSaveMetadata(
                    1,
                    steamId,
                    FixedSaveLength,
                    lastKnownSha256 ?? Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant(),
                    seed?.SeedId,
                    seed?.PlacementSha256,
                    cleanExit),
                JsonOptions));
        }

        public void CreateHardLinkedExternalRmmToNormal()
        {
            Directory.CreateDirectory(SaveDirectory);
            if (!CreateHardLinkW(DedicatedPath, SourcePath, IntPtr.Zero))
            {
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            }

            var bytes = File.ReadAllBytes(SourcePath);
            WriteMetadata(new DedicatedSaveMetadata(
                1,
                SteamId,
                FixedSaveLength,
                Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant(),
                null,
                null,
                true));
        }

        public DedicatedSaveService CreateService() =>
            new(Layout, Boundary, new SaveSelectionStore(Layout, Boundary), new SystemFileAccess());

        public DedicatedSaveMetadata ReadMetadata() =>
            JsonSerializer.Deserialize<DedicatedSaveMetadata>(
                File.ReadAllBytes(MetadataPath),
                JsonOptions)!;

        public void AssertArchiveContains(byte[] save, byte[] metadata)
        {
            var archive = Path.Combine(SaveDirectory, "archive");
            Assert.Contains(
                Directory.EnumerateFiles(archive, "*.rmm"),
                path => File.ReadAllBytes(path).SequenceEqual(save));
            Assert.Contains(
                Directory.EnumerateFiles(archive, "*.json"),
                path => File.ReadAllBytes(path).SequenceEqual(metadata));
        }

        public SourceState CaptureSourceState()
        {
            var info = new FileInfo(SourcePath);
            return new SourceState(
                Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(SourcePath))),
                info.Length,
                info.LastWriteTimeUtc,
                info.Attributes);
        }

        private void WriteMetadata(DedicatedSaveMetadata metadata) =>
            File.WriteAllBytes(MetadataPath, JsonSerializer.SerializeToUtf8Bytes(metadata, JsonOptions));
    }

    private sealed record SourceState(
        string ContentSha256,
        long Length,
        DateTime LastWriteTimeUtc,
        FileAttributes Attributes);

    private sealed record OpenObservation(
        string Path,
        FileMode Mode,
        FileAccess Access,
        FileShare Share);

    private enum CopyBehavior
    {
        Normal,
        Short,
        CorruptSameLength
    }

    private sealed class RecordingFileAccess(IFileAccess inner) : IFileAccess
    {
        public List<OpenObservation> Opens { get; } = [];
        public CopyBehavior CopyBehavior { get; set; }
        public Action<string>? AfterCopy { get; set; }
        public string? RaceDestinationPath { get; set; }
        public byte[]? RaceDestinationBytes { get; set; }
        public bool FailNextMetadataReplace { get; set; }
        public bool FailSaveArchiveMoves { get; set; }
        public bool FailMetadataWriteAfterCreate { get; set; }
        public byte[]? ReplaceMetadataTemporaryBeforeWriteFailure { get; set; }
        public string? ReplacedMetadataTemporaryPath { get; private set; }
        public bool MetadataReplaceCompleted { get; private set; }
        public string? CorruptOnHashPath { get; set; }
        public int CorruptOnHashPathCall { get; set; }
        public Action<string, int>? BeforePathIdentityAndHash { get; set; }
        public Action<string, string>? AfterMoveCreateNew { get; set; }
        public Action<string, string>? BeforeMoveCreateNew { get; set; }
        public Action<string>? BeforeOwnedDelete { get; set; }
        public Action<string>? AfterMutationLeaseAcquired { get; set; }
        public Action<string, bool>? AfterSingleLinkCheck { get; set; }
        public bool ParentSwapAttempted { get; set; }
        public string? RedirectFrom { get; set; }
        public string? RedirectTo { get; set; }
        public bool RedirectedMutationObserved { get; private set; }
        private Dictionary<string, int> HashPathCounts { get; } =
            new(StringComparer.OrdinalIgnoreCase);

        public bool Exists(string path) => inner.Exists(Map(path));

        public IFileMutationLease AcquireMutationLease(
            string rootPath,
            IReadOnlyCollection<string> directoryPaths)
        {
            var lease = inner.AcquireMutationLease(
                Map(rootPath),
                directoryPaths.Select(Map).ToArray());
            try
            {
                foreach (var directoryPath in directoryPaths)
                {
                    AfterMutationLeaseAcquired?.Invoke(directoryPath);
                }

                return lease;
            }
            catch
            {
                lease.Dispose();
                throw;
            }
        }

        public IFileMutationLease AcquireSessionLock(string rootPath, string lockPath) =>
            inner.AcquireSessionLock(Map(rootPath), Map(lockPath));

        public FileAttributes GetAttributes(string path) => inner.GetAttributes(Map(path));

        public bool IsSingleLinkFile(string path)
        {
            var isSingleLink = inner.IsSingleLinkFile(Map(path));
            AfterSingleLinkCheck?.Invoke(path, isSingleLink);
            return isSingleLink;
        }

        public Stream Open(string path, FileMode mode, FileAccess access, FileShare share)
        {
            Opens.Add(new OpenObservation(Path.GetFullPath(path), mode, access, share));
            return inner.Open(Map(path), mode, access, share);
        }

        public Task<FileIdentityAndHash> IdentityAndHashAsync(
            Stream stream,
            CancellationToken cancellationToken) =>
            inner.IdentityAndHashAsync(stream, cancellationToken);

        public Task<FileIdentityAndHash> IdentityAndHashAsync(
            string path,
            CancellationToken cancellationToken)
        {
            HashPathCounts.TryGetValue(path, out var count);
            count++;
            HashPathCounts[path] = count;
            BeforePathIdentityAndHash?.Invoke(path, count);
            if (count == CorruptOnHashPathCall
                && path.Equals(CorruptOnHashPath, StringComparison.OrdinalIgnoreCase))
            {
                var bytes = File.ReadAllBytes(path);
                bytes[0] ^= 0xFF;
                File.WriteAllBytes(path, bytes);
            }

            return inner.IdentityAndHashAsync(Map(path), cancellationToken);
        }

        public async Task<CreatedFileIdentity> CopyAndFlushAsync(
            Stream source,
            string destinationPath,
            CancellationToken cancellationToken)
        {
            CreatedFileIdentity created;
            if (CopyBehavior == CopyBehavior.Normal)
            {
                ObserveMutation(destinationPath);
                created = await inner.CopyAndFlushAsync(
                    source,
                    Map(destinationPath),
                    cancellationToken);
            }
            else
            {
                source.Position = 0;
                await using (var destination = inner.Open(
                                 Map(destinationPath),
                                 FileMode.CreateNew,
                                 FileAccess.Write,
                                 FileShare.None))
                {
                    var remaining = CopyBehavior == CopyBehavior.Short
                        ? Math.Max(0, source.Length - 1)
                        : source.Length;
                    var buffer = new byte[81920];
                    while (remaining > 0)
                    {
                        var read = await source.ReadAsync(
                            buffer.AsMemory(0, (int)Math.Min(buffer.Length, remaining)),
                            cancellationToken);
                        if (read == 0)
                        {
                            break;
                        }

                        await destination.WriteAsync(buffer.AsMemory(0, read), cancellationToken);
                        remaining -= read;
                    }

                    if (CopyBehavior == CopyBehavior.CorruptSameLength)
                    {
                        destination.Position = 0;
                        await destination.WriteAsync(new byte[] { 0xFF }, cancellationToken);
                    }

                    await destination.FlushAsync(cancellationToken);
                }
                var identity = await inner.IdentityAndHashAsync(
                    Map(destinationPath),
                    cancellationToken);
                created = new CreatedFileIdentity(identity.Identity);
            }

            AfterCopy?.Invoke(destinationPath);
            return created;
        }

        public async Task<CreatedFileIdentity> WriteAllBytesAndFlushAsync(
            string path,
            ReadOnlyMemory<byte> bytes,
            CancellationToken cancellationToken)
        {
            ObserveMutation(path);
            var mappedPath = Map(path);
            var created = await inner.WriteAllBytesAndFlushAsync(
                mappedPath,
                bytes,
                cancellationToken);
            if (ReplaceMetadataTemporaryBeforeWriteFailure is not null
                && Path.GetFileName(path).StartsWith("save-metadata.", StringComparison.OrdinalIgnoreCase))
            {
                File.Delete(mappedPath);
                File.WriteAllBytes(mappedPath, ReplaceMetadataTemporaryBeforeWriteFailure);
                ReplacedMetadataTemporaryPath = mappedPath;
                ReplaceMetadataTemporaryBeforeWriteFailure = null;
                inner.DeleteIfIdentityMatches(mappedPath, created.Identity);
                throw new IOException("Injected metadata write failure after foreign replacement.");
            }

            if (FailMetadataWriteAfterCreate
                && Path.GetFileName(path).StartsWith("save-metadata.", StringComparison.OrdinalIgnoreCase))
            {
                FailMetadataWriteAfterCreate = false;
                inner.DeleteIfIdentityMatches(mappedPath, created.Identity);
                throw new IOException("Injected metadata write failure after creation.");
            }

            return created;
        }

        public bool MoveCreateNewIfIdentityMatches(
            string sourcePath,
            string destinationPath,
            string expectedSourceIdentity)
        {
            BeforeMoveCreateNew?.Invoke(sourcePath, destinationPath);
            ObserveMutation(destinationPath);
            if (FailSaveArchiveMoves
                && sourcePath.EndsWith("DRAKS0005.rmm", StringComparison.OrdinalIgnoreCase)
                && destinationPath.Contains("archive", StringComparison.OrdinalIgnoreCase))
            {
                throw new IOException("Injected persistent save archive failure.");
            }
            var mappedSource = Map(sourcePath);
            var mappedDestination = Map(destinationPath);
            if (destinationPath.Equals(RaceDestinationPath, StringComparison.OrdinalIgnoreCase))
            {
                Directory.CreateDirectory(Path.GetDirectoryName(mappedDestination)!);
                File.WriteAllBytes(mappedDestination, RaceDestinationBytes!);
                throw new IOException("Injected destination race.");
            }

            var moved = inner.MoveCreateNewIfIdentityMatches(
                mappedSource,
                mappedDestination,
                expectedSourceIdentity);
            if (moved)
            {
                AfterMoveCreateNew?.Invoke(sourcePath, destinationPath);
            }

            return moved;
        }

        public bool ReplaceIfSourceIdentityMatches(
            string sourcePath,
            string destinationPath,
            string expectedSourceIdentity)
        {
            if (FailNextMetadataReplace
                && destinationPath.EndsWith("save-metadata.json", StringComparison.OrdinalIgnoreCase))
            {
                FailNextMetadataReplace = false;
                throw new IOException("Injected metadata publish failure.");
            }

            ObserveMutation(destinationPath);
            var replaced = inner.ReplaceIfSourceIdentityMatches(
                Map(sourcePath),
                Map(destinationPath),
                expectedSourceIdentity);
            if (destinationPath.EndsWith("save-metadata.json", StringComparison.OrdinalIgnoreCase))
            {
                MetadataReplaceCompleted = true;
            }

            return replaced;
        }

        public bool DeleteIfIdentityMatches(string path, string expectedIdentity)
        {
            BeforeOwnedDelete?.Invoke(path);
            ObserveMutation(path);
            return inner.DeleteIfIdentityMatches(Map(path), expectedIdentity);
        }

        private void ObserveMutation(string path)
        {
            if (!Map(path).Equals(path, StringComparison.OrdinalIgnoreCase))
            {
                RedirectedMutationObserved = true;
            }
        }

        private string Map(string path)
        {
            if (RedirectFrom is null || RedirectTo is null)
            {
                return path;
            }

            var fullPath = Path.GetFullPath(path);
            if (!fullPath.Equals(RedirectFrom, StringComparison.OrdinalIgnoreCase)
                && !fullPath.StartsWith(
                    RedirectFrom.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar,
                    StringComparison.OrdinalIgnoreCase))
            {
                return path;
            }

            return Path.Combine(RedirectTo, Path.GetRelativePath(RedirectFrom, fullPath));
        }
    }

    private sealed class EscapingCanonicalizer : IPathCanonicalizer
    {
        public string? EscapePrefix { get; set; }
        public string? EscapeTarget { get; set; }

        public string Canonicalize(string path)
        {
            var fullPath = Path.GetFullPath(path).TrimEnd(Path.DirectorySeparatorChar);
            if (EscapePrefix is not null
                && EscapeTarget is not null
                && (fullPath.Equals(EscapePrefix, StringComparison.OrdinalIgnoreCase)
                    || fullPath.StartsWith(
                        EscapePrefix.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar,
                        StringComparison.OrdinalIgnoreCase)))
            {
                var relative = Path.GetRelativePath(EscapePrefix, fullPath);
                return Path.GetFullPath(Path.Combine(EscapeTarget, relative));
            }

            return fullPath;
        }
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CreateHardLinkW(
        string fileName,
        string existingFileName,
        IntPtr securityAttributes);
}
