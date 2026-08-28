using System.Buffers.Binary;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text.Json;
using DSRRandomizer.Foundation.Paths;
using DSRRandomizer.Foundation.Runtime;
using DSRRandomizer.Foundation.Safety;
using DSRRandomizer.Foundation.Saves;
using DSRRandomizer.Launcher.Native;
using DSRRandomizer.Launcher.Safety;
using DSRRandomizer.Launcher.Services;

namespace DSRRandomizer.Launcher.Tests.Services;

public sealed class LauncherServiceTests : IDisposable
{
    private const long FixedSaveLength = 4_326_608;
    private const string SteamId = "12345678901234567";
    private static readonly string[] RequiredDirectories =
    {
        "chr", "event", "facegen", "font", "map", "menu", "movww", "msg",
        "mtd", "obj", "other", "param", "paramdef", "parts", "remo", "script",
        "sfx", "shader", "sound"
    };

    private readonly string _container = Path.Combine(
        Path.GetTempPath(),
        $"dsr-launcher-service-{Guid.NewGuid():N}");

    [Fact]
    public async Task InitializeRuntimeAsync_PersistsSelectionForFreshStatusServiceWithoutChangingSource()
    {
        var source = Path.Combine(_container, "source");
        var local = Path.Combine(_container, "local");
        CreateFakeInstallation(source);
        Directory.CreateDirectory(local);
        var before = CaptureSource(source);
        var service = new LauncherService(local);

        var manifest = await service.InitializeRuntimeAsync(
            source,
            progress: null,
            CancellationToken.None);
        var readinessFromFreshService = await new LauncherService(local)
            .GetReadinessAsync(CancellationToken.None);

        Assert.True(readinessFromFreshService.IsReady, string.Join(Environment.NewLine, readinessFromFreshService.Errors));
        Assert.Equal(manifest.RuntimePath, readinessFromFreshService.RuntimePath);
        Assert.False(File.Exists(Path.Combine(manifest.RuntimePath, "d3d11.dll")));
        Assert.Equal(before, CaptureSource(source));
    }

    [Fact]
    public async Task DiscoverSaveProfilesAsync_ReturnsProfilesFromConfiguredDocumentsFolder()
    {
        var documents = Path.Combine(_container, "documents");
        var source = CreateNormalSave(documents, SteamId, 0x31);
        var service = new LauncherService(
            Path.Combine(_container, "local"),
            new FixedKnownFolderProvider(documents));

        var profiles = await service.DiscoverSaveProfilesAsync(CancellationToken.None);

        var profile = Assert.Single(profiles);
        Assert.Equal(SteamId, profile.SteamId);
        Assert.Equal(Path.GetFullPath(source), profile.SourcePath);
    }

    [Fact]
    public async Task DiscoverSaveProfilesAsync_ExternalOnlyRmmIsReusableWithoutNormalRoot()
    {
        const string shortSteamId = "146808034";
        var documents = Path.Combine(_container, "missing-documents");
        var local = Path.Combine(_container, "local");
        var destination = CreateValidDedicatedSave(local, shortSteamId, 0x52);
        var service = new LauncherService(local, new FixedKnownFolderProvider(documents));

        var profiles = await service.DiscoverSaveProfilesAsync(CancellationToken.None);
        var profile = Assert.Single(profiles);
        var result = await service.PrepareDedicatedSaveAsync(
            profile.SteamId,
            firstCopyConfirmed: false,
            CancellationToken.None);

        Assert.Equal(shortSteamId, profile.SteamId);
        Assert.Equal(string.Empty, profile.SourcePath);
        Assert.True(result.Ready, result.Message);
        Assert.True(result.ReusedExisting);
        Assert.Equal(destination, result.SavePath);
        Assert.False(Directory.Exists(documents));
    }

    [Fact]
    public async Task NormalSaveBlockingFileAccess_RejectsEveryPathBasedReadOfExactNormalSave()
    {
        var normalSave = Path.Combine(_container, "DRAKS0005.sl2");
        Directory.CreateDirectory(_container);
        await File.WriteAllBytesAsync(normalSave, [0x31]);
        var access = new NormalSaveBlockingFileAccess(new SystemFileAccess());

        Assert.False(access.Exists(normalSave));
        var attributesFailure = Assert.Throws<UnauthorizedAccessException>(
            () => access.GetAttributes(normalSave));
        Assert.DoesNotContain("confirmation", attributesFailure.Message, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("session", attributesFailure.Message, StringComparison.OrdinalIgnoreCase);
        Assert.Throws<UnauthorizedAccessException>(() =>
            access.Open(normalSave, FileMode.Open, FileAccess.Read, FileShare.Read));
        await Assert.ThrowsAsync<UnauthorizedAccessException>(() =>
            access.IdentityAndHashAsync(normalSave, CancellationToken.None));
    }

    [Fact]
    public async Task PrepareDedicatedSaveAsync_UnconfirmedFirstCopyDoesNotOpenNormalSave()
    {
        var documents = Path.Combine(_container, "documents");
        var source = CreateNormalSave(documents, SteamId, 0x31);
        var service = new LauncherService(
            Path.Combine(_container, "local"),
            new FixedKnownFolderProvider(documents));
        await using var exclusiveSource = new FileStream(
            source,
            FileMode.Open,
            FileAccess.Read,
            FileShare.None);

        var result = await service.PrepareDedicatedSaveAsync(
            SteamId,
            firstCopyConfirmed: false,
            CancellationToken.None);

        Assert.False(result.Ready);
        Assert.Equal(SaveErrorCode.FirstCopyConfirmationRequired, result.ErrorCode);
        Assert.Contains("confirmation", result.Message, StringComparison.OrdinalIgnoreCase);
        Assert.False(File.Exists(Path.Combine(
            _container,
            "local",
            "saves",
            SteamId,
            "DRAKS0005.rmm")));
    }

    [Fact]
    public async Task PrepareDedicatedSaveAsync_ConfirmedFirstCopyUsesExactSelectedProfile()
    {
        var documents = Path.Combine(_container, "documents");
        var source = CreateNormalSave(documents, SteamId, 0x42);
        var local = Path.Combine(_container, "local");
        var service = new LauncherService(local, new FixedKnownFolderProvider(documents));

        var result = await service.PrepareDedicatedSaveAsync(
            SteamId,
            firstCopyConfirmed: true,
            CancellationToken.None);

        Assert.True(result.Ready, result.Message);
        Assert.False(result.ReusedExisting);
        Assert.Equal(File.ReadAllBytes(source), File.ReadAllBytes(result.SavePath!));
    }

    [Fact]
    public async Task PrepareDedicatedSaveAsync_ExistingValidRmmNeedsNoConfirmationOrNormalSaveOpen()
    {
        var documents = Path.Combine(_container, "documents");
        var source = CreateNormalSave(documents, SteamId, 0x31);
        var local = Path.Combine(_container, "local");
        CreateValidDedicatedSave(local, SteamId, 0x52);
        var service = new LauncherService(local, new FixedKnownFolderProvider(documents));
        await using var exclusiveSource = new FileStream(
            source,
            FileMode.Open,
            FileAccess.Read,
            FileShare.None);

        var result = await service.PrepareDedicatedSaveAsync(
            SteamId,
            firstCopyConfirmed: false,
            CancellationToken.None);

        Assert.True(result.Ready, result.Message);
        Assert.True(result.ReusedExisting);
    }

    [Fact]
    public async Task PrepareDedicatedSaveAsync_UnconfirmedExistingSaveRaceNeverOpensStoredNormalSave()
    {
        var documents = Path.Combine(_container, "documents");
        var source = CreateNormalSave(documents, SteamId, 0x31);
        var local = Path.Combine(_container, "local");
        var destination = CreateValidDedicatedSave(local, SteamId, 0x52);
        CreateSelectedProfile(local, SteamId, source);
        var access = new DestinationDisappearingFileAccess(destination);
        var service = new LauncherService(
            local,
            new FixedKnownFolderProvider(documents),
            access);

        var result = await service.PrepareDedicatedSaveAsync(
            SteamId,
            firstCopyConfirmed: false,
            CancellationToken.None);

        Assert.False(result.Ready);
        Assert.Empty(access.NormalSaveOpens);
    }

    [Fact]
    public async Task LaunchModdedAsync_UsesCopiedExeDedicatedRmmAndExactSaveBitmap()
    {
        using var fixture = await LaunchFixture.CreateAsync(existingRmm: true);

        var result = await fixture.Service.LaunchModdedAsync(
            SteamId,
            CancellationToken.None);

        Assert.True(result.Started, result.ErrorCode);
        Assert.Equal(fixture.RuntimeExe, fixture.Platform.Request?.ExecutablePath);
        Assert.Equal(fixture.RuntimeRoot, fixture.Platform.Request?.WorkingDirectory);
        Assert.Equal(fixture.DedicatedRmm, fixture.Platform.Request?.SavePaths?.DedicatedRmm);
        Assert.StartsWith(
            fixture.ExternalRoot,
            fixture.Platform.Request?.SavePaths?.VirtualDocuments,
            StringComparison.OrdinalIgnoreCase);
        Assert.EndsWith(
            Path.Combine(SteamId, "DRAKS0005.sl2"),
            fixture.Platform.Request?.SavePaths?.VirtualLogicalSave,
            StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain(
            "overhaul",
            fixture.Platform.Request?.SavePaths?.VirtualLogicalSave,
            StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain(
            fixture.NormalSave,
            new[]
            {
                fixture.Platform.Request!.SavePaths!.VirtualLogicalSave,
                fixture.Platform.Request.SavePaths.DedicatedRmm
            },
            StringComparer.OrdinalIgnoreCase);
        Assert.Equal(0x3UL, fixture.Platform.Request?.RequiredProtectionFlags);
        Assert.Empty(fixture.Platform.Request?.Arguments ?? []);
        Assert.Equal(1, fixture.Platform.Process.ResumeCalls);
    }

    [Fact]
    public async Task LaunchModdedAsync_BindsRmmAsTemporarySl2HardLinkDuringGameOnly()
    {
        using var fixture = await LaunchFixture.CreateAsync(existingRmm: true);
        var alias = Path.Combine(
            fixture.ExternalRoot,
            "profile",
            "NBGI",
            "DARK SOULS REMASTERED",
            SteamId,
            "DRAKS0005.sl2");
        var aliasWasPresentDuringGame = false;
        var rmmWasLinkedDuringGame = false;
        fixture.Platform.Process.OnWaitForExit = () =>
        {
            aliasWasPresentDuringGame = File.Exists(alias);
            rmmWasLinkedDuringGame = !new SystemFileAccess()
                .IsSingleLinkFile(fixture.DedicatedRmm);
            using var stream = new FileStream(
                alias,
                FileMode.Open,
                FileAccess.Write,
                FileShare.ReadWrite);
            stream.Position = 0;
            stream.WriteByte(0x7b);
            stream.Flush(flushToDisk: true);
        };

        var result = await fixture.Service.LaunchModdedAsync(
            SteamId,
            CancellationToken.None);

        Assert.True(result.Started, result.ErrorCode);
        Assert.True(aliasWasPresentDuringGame);
        Assert.True(rmmWasLinkedDuringGame);
        Assert.Equal(0x7b, File.ReadAllBytes(fixture.DedicatedRmm)[0]);
        Assert.False(File.Exists(alias));
        Assert.True(new SystemFileAccess().IsSingleLinkFile(fixture.DedicatedRmm));
        Assert.True(fixture.ReadMetadata().CleanExit);
    }

    [Fact]
    public async Task LaunchModdedAsync_RemovesMatchingStaleAliasBeforeRmmValidation()
    {
        using var fixture = await LaunchFixture.CreateAsync(existingRmm: true);
        var alias = Path.Combine(
            fixture.ExternalRoot,
            "profile",
            "NBGI",
            "DARK SOULS REMASTERED",
            SteamId,
            "DRAKS0005.sl2");
        Directory.CreateDirectory(Path.GetDirectoryName(alias)!);
        Assert.True(CreateHardLink(alias, fixture.DedicatedRmm, IntPtr.Zero));

        var result = await fixture.Service.LaunchModdedAsync(
            SteamId,
            CancellationToken.None);

        Assert.True(result.Started, result.ErrorCode);
        Assert.False(File.Exists(alias));
        Assert.True(new SystemFileAccess().IsSingleLinkFile(fixture.DedicatedRmm));
    }

    [Fact]
    public async Task LaunchModdedAsync_ForeignVirtualSl2FailsWithoutChangingEitherSave()
    {
        using var fixture = await LaunchFixture.CreateAsync(existingRmm: true);
        var alias = Path.Combine(
            fixture.ExternalRoot,
            "profile",
            "NBGI",
            "DARK SOULS REMASTERED",
            SteamId,
            "DRAKS0005.sl2");
        Directory.CreateDirectory(Path.GetDirectoryName(alias)!);
        await File.WriteAllBytesAsync(alias, [0x11, 0x22, 0x33]);
        var aliasBefore = await File.ReadAllBytesAsync(alias);
        var rmmBefore = await File.ReadAllBytesAsync(fixture.DedicatedRmm);

        var result = await fixture.Service.LaunchModdedAsync(
            SteamId,
            CancellationToken.None);

        Assert.False(result.Started);
        Assert.Equal("DEDICATED_SAVE_ALIAS_CONFLICT", result.ErrorCode);
        Assert.Equal(0, fixture.Platform.CreateCalls);
        Assert.Equal(aliasBefore, await File.ReadAllBytesAsync(alias));
        Assert.Equal(rmmBefore, await File.ReadAllBytesAsync(fixture.DedicatedRmm));
    }

    [Fact]
    public async Task LaunchModdedAsync_MissingRmmBootstrapsSelectedNormalSaveBeforeProcessCreation()
    {
        using var fixture = await LaunchFixture.CreateAsync(existingRmm: false);
        var sourceBefore = await File.ReadAllBytesAsync(fixture.NormalSave);

        var result = await fixture.Service.LaunchModdedAsync(
            SteamId,
            CancellationToken.None);

        Assert.True(result.Started, result.ErrorCode);
        Assert.Equal(sourceBefore, await File.ReadAllBytesAsync(fixture.NormalSave));
        Assert.Equal(sourceBefore, await File.ReadAllBytesAsync(fixture.DedicatedRmm));
        Assert.Equal(1, fixture.FileAccess.NormalSaveOpenCount);
        Assert.Equal(1, fixture.Platform.NormalSaveOpenCountAtProcessCreation);
    }

    [Fact]
    public async Task LaunchModdedAsync_SteamIdSelectsUniqueNormalSaveWhenRmmIsMissing()
    {
        using var fixture = await LaunchFixture.CreateAsync(
            existingRmm: false,
            persistNormalSelection: false);

        var result = await fixture.Service.LaunchModdedAsync(
            SteamId,
            CancellationToken.None);

        Assert.True(result.Started, result.ErrorCode);
        Assert.Equal(1, fixture.FileAccess.NormalSaveOpenCount);
        Assert.True(File.Exists(fixture.DedicatedRmm));
    }

    [Fact]
    public async Task LaunchModdedAsync_ExistingRmmNeverOpensNormalSaveAndReusesItByteForByte()
    {
        using var fixture = await LaunchFixture.CreateAsync(existingRmm: true);
        var rmmBefore = await File.ReadAllBytesAsync(fixture.DedicatedRmm);
        await using var exclusiveNormal = new FileStream(
            fixture.NormalSave,
            FileMode.Open,
            FileAccess.Read,
            FileShare.None);

        var result = await fixture.Service.LaunchModdedAsync(
            SteamId,
            CancellationToken.None);

        Assert.True(result.Started, result.ErrorCode);
        Assert.Equal(0, fixture.FileAccess.NormalSaveOpenCount);
        Assert.Equal(rmmBefore, await File.ReadAllBytesAsync(fixture.DedicatedRmm));
    }

    [Fact]
    public async Task LaunchModdedAsync_ExistingRmmDisappearsBeforeSessionNeverFallsBackToNormalSave()
    {
        using var fixture = await LaunchFixture.CreateAsync(existingRmm: true);
        fixture.FileAccess.OnAcquireMutationLease = () =>
        {
            File.Delete(fixture.DedicatedRmm);
            File.Delete(Path.Combine(
                Path.GetDirectoryName(fixture.DedicatedRmm)!,
                "save-metadata.json"));
        };

        var result = await fixture.Service.LaunchModdedAsync(
            SteamId,
            CancellationToken.None);

        Assert.False(result.Started);
        Assert.Equal(0, fixture.FileAccess.NormalSaveOpenCount);
        Assert.Equal(0, fixture.Platform.CreateCalls);
        fixture.AssertSessionLockReleased();
    }

    [Fact]
    public async Task LaunchModdedAsync_ByteIdenticalRmmReplacementBeforeSessionFailsAndReleasesLock()
    {
        using var fixture = await LaunchFixture.CreateAsync(existingRmm: true);
        var originalBytes = await File.ReadAllBytesAsync(fixture.DedicatedRmm);
        fixture.FileAccess.OnAcquireMutationLease = () =>
        {
            var replacement = fixture.DedicatedRmm + ".replacement";
            File.WriteAllBytes(replacement, originalBytes);
            File.Move(replacement, fixture.DedicatedRmm, overwrite: true);
        };

        var result = await fixture.Service.LaunchModdedAsync(
            SteamId,
            CancellationToken.None);

        Assert.False(result.Started);
        Assert.Equal(0, fixture.FileAccess.NormalSaveOpenCount);
        Assert.Equal(0, fixture.Platform.CreateCalls);
        fixture.AssertSessionLockReleased();
    }

    [Fact]
    public async Task LaunchModdedAsync_MatchingSaveAndMetadataReplacementBeforeSessionFails()
    {
        using var fixture = await LaunchFixture.CreateAsync(existingRmm: true);
        var metadataPath = Path.Combine(
            Path.GetDirectoryName(fixture.DedicatedRmm)!,
            "save-metadata.json");
        var saveBytes = await File.ReadAllBytesAsync(fixture.DedicatedRmm);
        var metadataBytes = await File.ReadAllBytesAsync(metadataPath);
        fixture.FileAccess.OnAcquireMutationLease = () =>
        {
            var saveReplacement = fixture.DedicatedRmm + ".replacement";
            var metadataReplacement = metadataPath + ".replacement";
            File.WriteAllBytes(saveReplacement, saveBytes);
            File.WriteAllBytes(metadataReplacement, metadataBytes);
            File.Move(saveReplacement, fixture.DedicatedRmm, overwrite: true);
            File.Move(metadataReplacement, metadataPath, overwrite: true);
        };

        var result = await fixture.Service.LaunchModdedAsync(
            SteamId,
            CancellationToken.None);

        Assert.False(result.Started);
        Assert.Equal(0, fixture.FileAccess.NormalSaveOpenCount);
        Assert.Equal(0, fixture.Platform.CreateCalls);
        fixture.AssertSessionLockReleased();
    }

    [Fact]
    public async Task LaunchModdedAsync_GameSaveWriteIsCommittedForNextLaunch()
    {
        using var fixture = await LaunchFixture.CreateAsync(existingRmm: true);
        fixture.Platform.Process.OnWaitForExit = () =>
        {
            var bytes = File.ReadAllBytes(fixture.DedicatedRmm);
            bytes[0] ^= 0xff;
            File.WriteAllBytes(fixture.DedicatedRmm, bytes);
        };

        var first = await fixture.Service.LaunchModdedAsync(
            SteamId,
            CancellationToken.None);
        fixture.Platform.Process.OnWaitForExit = null;
        var second = await fixture.Service.LaunchModdedAsync(
            SteamId,
            CancellationToken.None);

        Assert.True(first.Started, first.ErrorCode);
        Assert.True(second.Started, second.ErrorCode);
        Assert.Equal(2, fixture.Platform.CreateCalls);
    }

    [Fact]
    public async Task LaunchModdedAsync_ReturnedFailureLeavesPreviousSaveBaselineUnchangedAndReleasesLock()
    {
        using var fixture = await LaunchFixture.CreateAsync(existingRmm: true);
        var baseline = fixture.ReadMetadata();
        fixture.Platform.Process.OnInject = _ =>
        {
            fixture.MutateDedicatedSave();
            fixture.FileAccess.FailNextLeaseVerification = true;
            return Task.FromResult(ProtectionHandshake.Failed("EXPECTED_HANDSHAKE_FAILURE"));
        };

        var result = await fixture.Service.LaunchModdedAsync(
            SteamId,
            CancellationToken.None);

        Assert.False(result.Started);
        Assert.Equal("EXPECTED_HANDSHAKE_FAILURE", result.ErrorCode);
        fixture.AssertAbnormalBaselineAndReleasedLock(baseline);
    }

    [Fact]
    public async Task LaunchModdedAsync_NonzeroExitLeavesPreviousSaveBaselineUnchangedAndReleasesLock()
    {
        using var fixture = await LaunchFixture.CreateAsync(existingRmm: true);
        var baseline = fixture.ReadMetadata();
        fixture.Platform.Process.OnWait = _ =>
        {
            fixture.MutateDedicatedSave();
            return Task.FromResult(17);
        };

        var result = await fixture.Service.LaunchModdedAsync(
            SteamId,
            CancellationToken.None);

        Assert.True(result.Started, result.ErrorCode);
        Assert.Equal(17, result.ExitCode);
        fixture.AssertAbnormalBaselineAndReleasedLock(baseline);
    }

    [Fact]
    public async Task LaunchModdedAsync_ThrownFailurePreservesOriginalWhenAbnormalCleanupFails()
    {
        using var fixture = await LaunchFixture.CreateAsync(existingRmm: true);
        var baseline = fixture.ReadMetadata();
        var expected = new InvalidOperationException("EXPECTED_LAUNCH_FAILURE");
        fixture.Platform.Process.OnInject = _ =>
        {
            fixture.MutateDedicatedSave();
            fixture.FileAccess.FailNextLeaseVerification = true;
            return Task.FromResult(ProtectionHandshake.Failed("EXPECTED_HANDSHAKE_FAILURE"));
        };
        fixture.Platform.Process.DisposeException = expected;

        var actual = await Assert.ThrowsAsync<InvalidOperationException>(() =>
            fixture.Service.LaunchModdedAsync(SteamId, CancellationToken.None));

        Assert.Same(expected, actual);
        fixture.AssertAbnormalBaselineAndReleasedLock(baseline);
    }

    [Fact]
    public async Task LaunchModdedAsync_CancellationLeavesPreviousSaveBaselineUnchangedAndReleasesLock()
    {
        using var fixture = await LaunchFixture.CreateAsync(existingRmm: true);
        var baseline = fixture.ReadMetadata();
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();
        fixture.Platform.Process.OnWait = _ =>
        {
            fixture.MutateDedicatedSave();
            fixture.FileAccess.FailNextLeaseVerification = true;
            return Task.FromCanceled<int>(cancellation.Token);
        };

        await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            fixture.Service.LaunchModdedAsync(SteamId, CancellationToken.None));

        fixture.AssertAbnormalBaselineAndReleasedLock(baseline);
    }

    [Fact]
    public async Task LaunchModdedAsync_BothSavesMissingFailsBeforeProcessCreation()
    {
        using var fixture = await LaunchFixture.CreateAsync(
            existingRmm: false,
            existingNormalSave: false);

        var result = await fixture.Service.LaunchModdedAsync(
            SteamId,
            CancellationToken.None);

        Assert.False(result.Started);
        Assert.Equal(0, fixture.Platform.CreateCalls);
    }

    [Fact]
    public async Task LaunchModdedAsync_InvalidBootstrapSourceFailsBeforeProcessCreation()
    {
        using var fixture = await LaunchFixture.CreateAsync(existingRmm: false);
        await File.WriteAllTextAsync(fixture.NormalSave, "invalid-save-length");

        var result = await fixture.Service.LaunchModdedAsync(
            SteamId,
            CancellationToken.None);

        Assert.False(result.Started);
        Assert.Equal(0, fixture.Platform.CreateCalls);
        Assert.False(File.Exists(fixture.DedicatedRmm));
    }

    [Theory]
    [InlineData("DarkSoulsRemastered.exe")]
    [InlineData("steam_api64.dll")]
    public async Task LaunchModdedAsync_ChangedProtectedCoreFailsBeforeProcessCreation(
        string relativePath)
    {
        using var fixture = await LaunchFixture.CreateAsync(existingRmm: true);
        await File.AppendAllTextAsync(Path.Combine(fixture.RuntimeRoot, relativePath), "changed");

        var result = await fixture.Service.LaunchModdedAsync(
            SteamId,
            CancellationToken.None);

        Assert.False(result.Started);
        Assert.Equal(0, fixture.Platform.CreateCalls);
    }

    [Fact]
    public async Task LaunchModdedAsync_UnsupportedProfileFailsBeforeProcessCreation()
    {
        using var fixture = await LaunchFixture.CreateAsync(
            existingRmm: true,
            supportedProfile: false);

        var result = await fixture.Service.LaunchModdedAsync(
            SteamId,
            CancellationToken.None);

        Assert.False(result.Started);
        Assert.Equal(0, fixture.Platform.CreateCalls);
    }

    [Fact]
    public async Task LaunchModdedAsync_MissingGuardFailsBeforeProcessCreation()
    {
        using var fixture = await LaunchFixture.CreateAsync(existingRmm: true);
        File.Delete(fixture.GuardDll);

        var result = await fixture.Service.LaunchModdedAsync(
            SteamId,
            CancellationToken.None);

        Assert.False(result.Started);
        Assert.Equal(0, fixture.Platform.CreateCalls);
    }

    [Fact]
    public async Task LaunchModdedAsync_ChangedGuardHashFailsBeforeProcessCreation()
    {
        using var fixture = await LaunchFixture.CreateAsync(existingRmm: true);
        await File.AppendAllTextAsync(fixture.GuardDll, "tampered");

        var result = await fixture.Service.LaunchModdedAsync(
            SteamId,
            CancellationToken.None);

        Assert.False(result.Started);
        Assert.Equal(0, fixture.Platform.CreateCalls);
    }

    [Fact]
    public async Task LaunchModdedAsync_ReplacingGuardAndInformationalSidecarTogetherFailsBeforeProcessCreation()
    {
        using var fixture = await LaunchFixture.CreateAsync(existingRmm: true);
        var replacement = LaunchFixture.SyntheticPe(0x71);
        await File.WriteAllBytesAsync(fixture.GuardDll, replacement);
        await File.WriteAllTextAsync(fixture.GuardHashPath, LaunchFixture.Sha256(replacement));

        var result = await fixture.Service.LaunchModdedAsync(SteamId, CancellationToken.None);

        Assert.False(result.Started);
        Assert.Equal("GUARD_ARTIFACT_INVALID", result.ErrorCode);
        Assert.Equal(0, fixture.Platform.CreateCalls);
    }

    [Fact]
    public async Task LaunchModdedAsync_MutatedCompatibilityProfileFailsBeforeProcessCreation()
    {
        using var fixture = await LaunchFixture.CreateAsync(existingRmm: true);
        await File.AppendAllTextAsync(fixture.ProfilePath, "tampered");

        var result = await fixture.Service.LaunchModdedAsync(SteamId, CancellationToken.None);

        Assert.False(result.Started);
        Assert.Equal("PROFILE_ARTIFACT_INVALID", result.ErrorCode);
        Assert.Equal(0, fixture.Platform.CreateCalls);
    }

    [Theory]
    [InlineData("DarkSoulsRemastered.exe", "create")]
    [InlineData("DSRRandomizer.Runtime.dll", "inject")]
    public async Task LaunchModdedAsync_SubstitutionAtProcessBoundaryIsBlockedAndCannotResume(
        string artifact,
        string boundary)
    {
        using var fixture = await LaunchFixture.CreateAsync(existingRmm: true);
        var path = artifact == "DarkSoulsRemastered.exe"
            ? fixture.RuntimeExe
            : fixture.GuardDll;
        var substitutionBlocked = false;
        void AttemptSubstitution() => substitutionBlocked = !TryReplace(
            path,
            LaunchFixture.SyntheticPe(0x72));
        if (boundary == "create")
        {
            fixture.Platform.OnCreate = AttemptSubstitution;
            fixture.Platform.Process.OnInject = _ => Task.FromResult(
                ProtectionHandshake.Failed("EXPECTED_BOUNDARY_FAILURE"));
        }
        else
        {
            fixture.Platform.Process.OnInject = _ =>
            {
                AttemptSubstitution();
                return Task.FromResult(ProtectionHandshake.Failed("EXPECTED_BOUNDARY_FAILURE"));
            };
        }

        var result = await fixture.Service.LaunchModdedAsync(SteamId, CancellationToken.None);

        Assert.True(substitutionBlocked);
        Assert.False(result.Started);
        Assert.Equal("EXPECTED_BOUNDARY_FAILURE", result.ErrorCode);
        Assert.Equal(0, fixture.Platform.Process.ResumeCalls);
    }

    [Fact]
    public async Task LaunchModdedAsync_RetainsSteamModuleLockUntilChildExit()
    {
        using var fixture = await LaunchFixture.CreateAsync(existingRmm: true);
        var blockedDuringWait = false;
        fixture.Platform.Process.OnWait = _ =>
        {
            blockedDuringWait = !TryReplace(
                fixture.SteamDll,
                LaunchFixture.SyntheticPe(0x73));
            return Task.FromResult(0);
        };

        var result = await fixture.Service.LaunchModdedAsync(SteamId, CancellationToken.None);

        Assert.True(result.Started, result.ErrorCode);
        Assert.True(blockedDuringWait);
        Assert.True(TryReplace(fixture.SteamDll, LaunchFixture.SyntheticPe(0x74)));
    }

    [Fact]
    public async Task LaunchModdedAsync_RejectsHardLinkedCompatibilityProfile()
    {
        using var fixture = await LaunchFixture.CreateAsync(existingRmm: true);
        var alias = Path.Combine(fixture.Container, "profile-hardlink.json");
        Assert.True(CreateHardLink(alias, fixture.ProfilePath, IntPtr.Zero));

        var result = await fixture.Service.LaunchModdedAsync(SteamId, CancellationToken.None);

        Assert.False(result.Started);
        Assert.Equal(0, fixture.Platform.CreateCalls);
    }

    [Fact]
    public async Task LaunchModdedAsync_RejectsCompatibilityProfileBelowReparseParent()
    {
        using var fixture = await LaunchFixture.CreateAsync(existingRmm: true);
        fixture.ReplaceProfileParentWithJunction();
        try
        {
            var result = await fixture.Service.LaunchModdedAsync(SteamId, CancellationToken.None);

            Assert.False(result.Started);
            Assert.Equal(0, fixture.Platform.CreateCalls);
        }
        finally
        {
            fixture.RemoveProfileParentJunction();
        }
    }

    [Fact]
    public async Task LaunchModdedAsync_RuntimeUsedAsSourceFailsBeforeProcessCreation()
    {
        using var fixture = await LaunchFixture.CreateAsync(existingRmm: true);
        await fixture.PointSourceAtRuntimeAsync();

        var result = await fixture.Service.LaunchModdedAsync(
            SteamId,
            CancellationToken.None);

        Assert.False(result.Started);
        Assert.Equal(0, fixture.Platform.CreateCalls);
    }

    public void Dispose()
    {
        if (Directory.Exists(_container))
        {
            Directory.Delete(_container, recursive: true);
        }
    }

    private static bool TryReplace(string path, byte[] replacement)
    {
        var temporary = path + $".{Guid.NewGuid():N}.replacement";
        File.WriteAllBytes(temporary, replacement);
        try
        {
            File.Move(temporary, path, overwrite: true);
            return true;
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            return false;
        }
        finally
        {
            if (File.Exists(temporary))
            {
                File.Delete(temporary);
            }
        }
    }

    private static void RunCmd(string arguments)
    {
        using var process = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo
        {
            FileName = "cmd.exe",
            Arguments = $"/c {arguments}",
            CreateNoWindow = true,
            UseShellExecute = false
        }) ?? throw new InvalidOperationException("Unable to start junction helper.");
        process.WaitForExit();
        Assert.Equal(0, process.ExitCode);
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CreateHardLink(
        string fileName,
        string existingFileName,
        IntPtr securityAttributes);

    private static void CreateFakeInstallation(string source)
    {
        Directory.CreateDirectory(source);
        foreach (var directory in RequiredDirectories)
        {
            Directory.CreateDirectory(Path.Combine(source, directory));
        }

        File.WriteAllText(Path.Combine(source, "DarkSoulsRemastered.exe"), "game");
        File.WriteAllText(Path.Combine(source, "map", "test.dcx"), "map");
        File.WriteAllText(Path.Combine(source, "d3d11.dll"), "installed overhaul loader");
    }

    private static string CreateNormalSave(string documents, string steamId, byte fill)
    {
        var profile = Path.Combine(
            documents,
            "NBGI",
            "DARK SOULS REMASTERED",
            steamId);
        Directory.CreateDirectory(profile);
        var path = Path.Combine(profile, "DRAKS0005.sl2");
        File.WriteAllBytes(path, Enumerable.Repeat(fill, checked((int)FixedSaveLength)).ToArray());
        return path;
    }

    private static string CreateValidDedicatedSave(string local, string steamId, byte fill)
    {
        var directory = Path.Combine(local, "saves", steamId);
        Directory.CreateDirectory(directory);
        var savePath = Path.Combine(directory, "DRAKS0005.rmm");
        var bytes = Enumerable.Repeat(fill, checked((int)FixedSaveLength)).ToArray();
        File.WriteAllBytes(savePath, bytes);
        var metadata = new DedicatedSaveMetadata(
            1,
            steamId,
            FixedSaveLength,
            Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant(),
            null,
            null,
            true);
        File.WriteAllBytes(
            Path.Combine(directory, "save-metadata.json"),
            JsonSerializer.SerializeToUtf8Bytes(metadata, new JsonSerializerOptions
            {
                PropertyNamingPolicy = JsonNamingPolicy.CamelCase
            }));
        return savePath;
    }

    private static void CreateSelectedProfile(string local, string steamId, string source)
    {
        var config = Path.Combine(local, "config");
        Directory.CreateDirectory(config);
        File.WriteAllBytes(
            Path.Combine(config, "selected-save-profile.json"),
            JsonSerializer.SerializeToUtf8Bytes(
                new SaveProfileCandidate(steamId, source),
                new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.CamelCase }));
    }

    private static string CaptureSource(string source) => string.Join(
        Environment.NewLine,
        Directory.EnumerateFiles(source, "*", SearchOption.AllDirectories)
            .Order(StringComparer.Ordinal)
            .Select(path =>
            {
                var info = new FileInfo(path);
                var hash = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path)));
                return $"{Path.GetRelativePath(source, path)}|{info.Length}|{info.LastWriteTimeUtc.Ticks}|{hash}";
            }));

    private sealed class LaunchFixture : IDisposable
    {
        private LaunchFixture(
            string container,
            string externalRoot,
            string sourceRoot,
            string runtimeRoot,
            string runtimeExe,
            string steamDll,
            string normalSave,
            string dedicatedRmm,
            string guardDll,
            string guardHashPath,
            string profilePath,
            TrackingFileAccess fileAccess,
            RecordingPlatform platform,
            LauncherService service)
        {
            Container = container;
            ExternalRoot = externalRoot;
            SourceRoot = sourceRoot;
            RuntimeRoot = runtimeRoot;
            RuntimeExe = runtimeExe;
            SteamDll = steamDll;
            NormalSave = normalSave;
            DedicatedRmm = dedicatedRmm;
            GuardDll = guardDll;
            GuardHashPath = guardHashPath;
            ProfilePath = profilePath;
            FileAccess = fileAccess;
            Platform = platform;
            Service = service;
        }

        public string Container { get; }
        public string ExternalRoot { get; }
        public string SourceRoot { get; }
        public string RuntimeRoot { get; }
        public string RuntimeExe { get; }
        public string SteamDll { get; }
        public string NormalSave { get; }
        public string DedicatedRmm { get; }
        public string GuardDll { get; }
        public string GuardHashPath { get; }
        public string ProfilePath { get; }
        public TrackingFileAccess FileAccess { get; }
        public RecordingPlatform Platform { get; }
        public LauncherService Service { get; }

        public DedicatedSaveMetadata ReadMetadata() => JsonSerializer.Deserialize<DedicatedSaveMetadata>(
            File.ReadAllBytes(Path.Combine(
                ExternalRoot,
                "saves",
                SteamId,
                "save-metadata.json")),
            new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.CamelCase })!;

        public void MutateDedicatedSave()
        {
            var bytes = File.ReadAllBytes(DedicatedRmm);
            bytes[0] ^= 0xff;
            File.WriteAllBytes(DedicatedRmm, bytes);
        }

        public void AssertAbnormalBaselineAndReleasedLock(DedicatedSaveMetadata baseline)
        {
            var after = ReadMetadata();
            Assert.False(after.CleanExit);
            Assert.Equal(baseline.LastKnownSha256, after.LastKnownSha256);
            using var lease = new SystemFileAccess().AcquireSessionLock(
                ExternalRoot,
                Path.Combine(ExternalRoot, "saves", SteamId, ".session.lock"));
            lease.Verify();
        }

        public void AssertSessionLockReleased()
        {
            using var lease = new SystemFileAccess().AcquireSessionLock(
                ExternalRoot,
                Path.Combine(ExternalRoot, "saves", SteamId, ".session.lock"));
            lease.Verify();
        }

        public Task PointSourceAtRuntimeAsync() => File.WriteAllTextAsync(
            Path.Combine(ExternalRoot, "config", "source-installation.json"),
            JsonSerializer.Serialize(new { canonicalInstallationPath = RuntimeRoot }));

        public void ReplaceProfileParentWithJunction()
        {
            var parent = Path.GetDirectoryName(ProfilePath)!;
            var target = parent + "-target";
            Directory.Move(parent, target);
            RunCmd($"mklink /J \"{parent}\" \"{target}\"");
        }

        public void RemoveProfileParentJunction()
        {
            var parent = Path.GetDirectoryName(ProfilePath)!;
            var target = parent + "-target";
            if (Directory.Exists(parent)
                && (new DirectoryInfo(parent).Attributes & FileAttributes.ReparsePoint) != 0)
            {
                Directory.Delete(parent);
            }
            if (Directory.Exists(target))
            {
                Directory.Move(target, parent);
            }
        }

        public static async Task<LaunchFixture> CreateAsync(
            bool existingRmm,
            bool existingNormalSave = true,
            bool supportedProfile = true,
            bool persistNormalSelection = true)
        {
            var container = Path.Combine(
                Path.GetTempPath(),
                $"dsr-launch-fixture-{Guid.NewGuid():N}");
            var externalRoot = Path.Combine(container, "external");
            var sourceRoot = Path.Combine(container, "source");
            var documents = Path.Combine(container, "documents");
            var runtimeId = "runtime-test";
            var runtimeRoot = Path.Combine(externalRoot, "runtimes", runtimeId);
            Directory.CreateDirectory(sourceRoot);
            Directory.CreateDirectory(runtimeRoot);

            var executableBytes = SyntheticPe(0x41);
            var steamBytes = SyntheticPe(0x52);
            var runtimeExe = Path.Combine(runtimeRoot, "DarkSoulsRemastered.exe");
            var steamDll = Path.Combine(runtimeRoot, "steam_api64.dll");
            await File.WriteAllBytesAsync(runtimeExe, executableBytes);
            await File.WriteAllBytesAsync(steamDll, steamBytes);

            var manifest = new RuntimeManifest(
                1,
                runtimeId,
                DateTimeOffset.UnixEpoch,
                Sha256(executableBytes),
                new string('a', 64),
                executableBytes.LongLength + steamBytes.LongLength,
                [
                    new RuntimeFileManifestEntry(
                        "DarkSoulsRemastered.exe",
                        executableBytes.LongLength,
                        Sha256(executableBytes)),
                    new RuntimeFileManifestEntry(
                        "steam_api64.dll",
                        steamBytes.LongLength,
                        Sha256(steamBytes))
                ]);
            var jsonOptions = new JsonSerializerOptions
            {
                PropertyNamingPolicy = JsonNamingPolicy.CamelCase
            };
            var manifestPath = Path.Combine(runtimeRoot, "runtime-manifest.json");
            await File.WriteAllBytesAsync(
                manifestPath,
                JsonSerializer.SerializeToUtf8Bytes(manifest, jsonOptions));
            await File.WriteAllBytesAsync(
                Path.Combine(externalRoot, "runtime-current.json"),
                JsonSerializer.SerializeToUtf8Bytes(
                    new RuntimePointer(
                        runtimeId,
                        Path.Combine("runtimes", runtimeId),
                        Sha256(await File.ReadAllBytesAsync(manifestPath))),
                    jsonOptions));
            var config = Path.Combine(externalRoot, "config");
            Directory.CreateDirectory(config);
            await File.WriteAllTextAsync(
                Path.Combine(config, "source-installation.json"),
                JsonSerializer.Serialize(
                    new { canonicalInstallationPath = Path.GetFullPath(sourceRoot) }));

            var normalSave = Path.Combine(
                documents,
                "NBGI",
                "DARK SOULS REMASTERED",
                SteamId,
                "DRAKS0005.sl2");
            if (existingNormalSave)
            {
                Directory.CreateDirectory(Path.GetDirectoryName(normalSave)!);
                await File.WriteAllBytesAsync(
                    normalSave,
                    Enumerable.Repeat((byte)0x31, checked((int)FixedSaveLength)).ToArray());
                if (persistNormalSelection)
                {
                    await File.WriteAllBytesAsync(
                        Path.Combine(config, "selected-save-profile.json"),
                        JsonSerializer.SerializeToUtf8Bytes(
                            new SaveProfileCandidate(SteamId, normalSave),
                            jsonOptions));
                }
            }

            var dedicatedRmm = Path.Combine(
                externalRoot,
                "saves",
                SteamId,
                "DRAKS0005.rmm");
            if (existingRmm)
            {
                CreateValidDedicatedSave(externalRoot, SteamId, 0x62);
            }

            var guardDirectory = Path.Combine(container, "package", "native");
            Directory.CreateDirectory(guardDirectory);
            var guardDll = Path.Combine(guardDirectory, "DSRRandomizer.Runtime.dll");
            var guardBytes = SyntheticPe(0x63);
            await File.WriteAllBytesAsync(guardDll, guardBytes);
            var guardHashPath = guardDll + ".sha256";
            await File.WriteAllTextAsync(guardHashPath, Sha256(guardBytes));
            var profileDirectory = Path.Combine(container, "package", "config");
            Directory.CreateDirectory(profileDirectory);
            var profilePath = Path.Combine(profileDirectory, "compatibility-profiles.json");
            var profileBytes = "synthetic pinned profile"u8.ToArray();
            await File.WriteAllBytesAsync(profilePath, profileBytes);

            var executableIdentity = ProfileInspector.InspectIdentity(runtimeExe);
            var steamIdentity = ProfileInspector.InspectIdentity(steamDll);
            var profile = new CompatibilityProfile(
                "synthetic-launch-profile",
                "DarkSoulsRemastered.exe",
                executableIdentity,
                FixedSaveLength,
                2,
                [new ModuleProfile(
                    "steam_api64.dll",
                    steamIdentity,
                    false,
                    Array.Empty<string>(),
                    Array.Empty<string>())],
                Array.Empty<InternalTargetProfile>());
            var catalog = new CompatibilityProfileCatalog(
                supportedProfile ? [profile] : Array.Empty<CompatibilityProfile>());
            var fileAccess = new TrackingFileAccess(normalSave);
            var platform = new RecordingPlatform(fileAccess);
            var service = new LauncherService(
                externalRoot,
                new FixedKnownFolderProvider(documents),
                fileAccess,
                catalog,
                platform,
                guardDll,
                profilePath,
                new LaunchArtifactIdentities(
                    Sha256(guardBytes),
                    Sha256(profileBytes)));
            return new LaunchFixture(
                container,
                externalRoot,
                sourceRoot,
                runtimeRoot,
                runtimeExe,
                steamDll,
                normalSave,
                dedicatedRmm,
                guardDll,
                guardHashPath,
                profilePath,
                fileAccess,
                platform,
                service);
        }

        public void Dispose() => Directory.Delete(Container, recursive: true);

        internal static byte[] SyntheticPe(byte fill)
        {
            var image = Enumerable.Repeat(fill, 0x1400).ToArray();
            image[0] = (byte)'M';
            image[1] = (byte)'Z';
            BinaryPrimitives.WriteInt32LittleEndian(image.AsSpan(0x3c), 0x80);
            image[0x80] = (byte)'P';
            image[0x81] = (byte)'E';
            image[0x82] = 0;
            image[0x83] = 0;
            BinaryPrimitives.WriteUInt16LittleEndian(image.AsSpan(0x84), 0x8664);
            BinaryPrimitives.WriteUInt16LittleEndian(image.AsSpan(0x86), 1);
            BinaryPrimitives.WriteUInt32LittleEndian(image.AsSpan(0x88), 0x6344ca56);
            BinaryPrimitives.WriteUInt16LittleEndian(image.AsSpan(0x94), 0xf0);
            BinaryPrimitives.WriteUInt16LittleEndian(image.AsSpan(0x98), 0x20b);
            BinaryPrimitives.WriteUInt32LittleEndian(image.AsSpan(0xd0), 0x3000);
            BinaryPrimitives.WriteUInt32LittleEndian(image.AsSpan(0xd4), 0x200);
            var section = image.AsSpan(0x188);
            ".text\0\0\0"u8.CopyTo(section);
            BinaryPrimitives.WriteUInt32LittleEndian(section[8..], 0x1000);
            BinaryPrimitives.WriteUInt32LittleEndian(section[12..], 0x1000);
            BinaryPrimitives.WriteUInt32LittleEndian(section[16..], 0x1000);
            BinaryPrimitives.WriteUInt32LittleEndian(section[20..], 0x200);
            BinaryPrimitives.WriteUInt32LittleEndian(section[36..], 0x60000020);
            return image;
        }

        internal static string Sha256(byte[] bytes) =>
            Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
    }

    private sealed class RecordingPlatform(TrackingFileAccess fileAccess) : IProtectedProcessPlatform
    {
        public SafetyLaunchRequest? Request { get; private set; }
        public int CreateCalls { get; private set; }
        public int NormalSaveOpenCountAtProcessCreation { get; private set; }
        public RecordingProcess Process { get; } = new();
        public Action? OnCreate { get; set; }

        public Task<IProtectedProcess> CreateSuspendedAsync(
            SafetyLaunchRequest request,
            CancellationToken cancellationToken)
        {
            OnCreate?.Invoke();
            CreateCalls++;
            Request = request;
            NormalSaveOpenCountAtProcessCreation = fileAccess.NormalSaveOpenCount;
            Process.RequiredFlags = request.RequiredProtectionFlags;
            return Task.FromResult<IProtectedProcess>(Process);
        }
    }

    private sealed class RecordingProcess : IProtectedProcess
    {
        public ulong RequiredFlags { get; set; }
        public Action? OnWaitForExit { get; set; }
        public Func<CancellationToken, Task<ProtectionHandshake>>? OnInject { get; set; }
        public Func<CancellationToken, Task<int>>? OnWait { get; set; }
        public Exception? DisposeException { get; set; }
        public int ResumeCalls { get; private set; }
        public int ProcessId => 1;
        public void AssignKillOnCloseJob() { }
        public Task<ProtectionHandshake> InjectAndInitializeAsync(CancellationToken cancellationToken) =>
            OnInject?.Invoke(cancellationToken)
            ?? Task.FromResult(new ProtectionHandshake(true, RequiredFlags, string.Empty));
        public uint ResumeMainThread()
        {
            ResumeCalls++;
            return 1;
        }
        public void TerminateJob() { }
        public Task<int> WaitForExitAsync(CancellationToken cancellationToken)
        {
            OnWaitForExit?.Invoke();
            return OnWait?.Invoke(cancellationToken) ?? Task.FromResult(0);
        }
        public ValueTask DisposeAsync() => DisposeException is null
            ? ValueTask.CompletedTask
            : ValueTask.FromException(DisposeException);
    }

    private sealed class TrackingFileAccess(string normalSave) : IFileAccess
    {
        private readonly IFileAccess _inner = new SystemFileAccess();
        public bool FailNextLeaseVerification { get; set; }
        public Action? OnAcquireMutationLease { get; set; }
        public int NormalSaveOpenCount { get; private set; }
        public bool Exists(string path) => _inner.Exists(path);
        public IFileMutationLease AcquireMutationLease(
            string rootPath,
            IReadOnlyCollection<string> directoryPaths)
        {
            var action = OnAcquireMutationLease;
            OnAcquireMutationLease = null;
            action?.Invoke();
            return new TrackingLease(_inner.AcquireMutationLease(rootPath, directoryPaths), this);
        }
        public IFileMutationLease AcquireSessionLock(string rootPath, string lockPath) =>
            _inner.AcquireSessionLock(rootPath, lockPath);
        public FileAttributes GetAttributes(string path) => _inner.GetAttributes(path);
        public bool IsSingleLinkFile(string path) => _inner.IsSingleLinkFile(path);
        public Stream Open(string path, FileMode mode, FileAccess access, FileShare share)
        {
            if (Path.GetFullPath(path).Equals(
                    Path.GetFullPath(normalSave),
                    StringComparison.OrdinalIgnoreCase))
            {
                NormalSaveOpenCount++;
            }
            return _inner.Open(path, mode, access, share);
        }
        public Task<FileIdentityAndHash> IdentityAndHashAsync(Stream stream, CancellationToken cancellationToken) =>
            _inner.IdentityAndHashAsync(stream, cancellationToken);
        public Task<FileIdentityAndHash> IdentityAndHashAsync(string path, CancellationToken cancellationToken) =>
            _inner.IdentityAndHashAsync(path, cancellationToken);
        public Task<CreatedFileIdentity> CopyAndFlushAsync(Stream source, string destinationPath, CancellationToken cancellationToken) =>
            _inner.CopyAndFlushAsync(source, destinationPath, cancellationToken);
        public Task<CreatedFileIdentity> WriteAllBytesAndFlushAsync(string path, ReadOnlyMemory<byte> bytes, CancellationToken cancellationToken) =>
            _inner.WriteAllBytesAndFlushAsync(path, bytes, cancellationToken);
        public bool MoveCreateNewIfIdentityMatches(string sourcePath, string destinationPath, string expectedSourceIdentity) =>
            _inner.MoveCreateNewIfIdentityMatches(sourcePath, destinationPath, expectedSourceIdentity);
        public bool ReplaceIfSourceIdentityMatches(string sourcePath, string destinationPath, string expectedSourceIdentity) =>
            _inner.ReplaceIfSourceIdentityMatches(sourcePath, destinationPath, expectedSourceIdentity);
        public bool DeleteIfIdentityMatches(string path, string expectedIdentity) =>
            _inner.DeleteIfIdentityMatches(path, expectedIdentity);

        private sealed class TrackingLease(
            IFileMutationLease inner,
            TrackingFileAccess owner) : IFileMutationLease
        {
            public void Verify()
            {
                if (owner.FailNextLeaseVerification)
                {
                    owner.FailNextLeaseVerification = false;
                    throw new InvalidOperationException("EXPECTED_ABNORMAL_CLEANUP_FAILURE");
                }
                inner.Verify();
            }

            public void Dispose() => inner.Dispose();
        }
    }

    private sealed class FixedKnownFolderProvider(string documents) : IKnownFolderProvider
    {
        public string GetDocumentsPath() => documents;
    }

    private sealed class DestinationDisappearingFileAccess(string destination) : IFileAccess
    {
        private readonly IFileAccess _inner = new SystemFileAccess();
        private bool _destinationRemoved;

        public List<string> NormalSaveOpens { get; } = [];

        public bool Exists(string path)
        {
            if (!_destinationRemoved
                && path.Equals(destination, StringComparison.OrdinalIgnoreCase))
            {
                _destinationRemoved = true;
                File.Delete(destination);
                File.Delete(Path.Combine(Path.GetDirectoryName(destination)!, "save-metadata.json"));
                return false;
            }

            return _inner.Exists(path);
        }

        public IFileMutationLease AcquireMutationLease(
            string rootPath,
            IReadOnlyCollection<string> directoryPaths) =>
            _inner.AcquireMutationLease(rootPath, directoryPaths);

        public IFileMutationLease AcquireSessionLock(string rootPath, string lockPath) =>
            _inner.AcquireSessionLock(rootPath, lockPath);

        public FileAttributes GetAttributes(string path) => _inner.GetAttributes(path);

        public bool IsSingleLinkFile(string path) => _inner.IsSingleLinkFile(path);

        public Stream Open(string path, FileMode mode, FileAccess access, FileShare share)
        {
            if (path.EndsWith(".sl2", StringComparison.OrdinalIgnoreCase))
            {
                NormalSaveOpens.Add(path);
            }

            return _inner.Open(path, mode, access, share);
        }

        public Task<FileIdentityAndHash> IdentityAndHashAsync(
            Stream stream,
            CancellationToken cancellationToken) =>
            _inner.IdentityAndHashAsync(stream, cancellationToken);

        public Task<FileIdentityAndHash> IdentityAndHashAsync(
            string path,
            CancellationToken cancellationToken) =>
            _inner.IdentityAndHashAsync(path, cancellationToken);

        public Task<CreatedFileIdentity> CopyAndFlushAsync(
            Stream source,
            string destinationPath,
            CancellationToken cancellationToken) =>
            _inner.CopyAndFlushAsync(source, destinationPath, cancellationToken);

        public Task<CreatedFileIdentity> WriteAllBytesAndFlushAsync(
            string path,
            ReadOnlyMemory<byte> bytes,
            CancellationToken cancellationToken) =>
            _inner.WriteAllBytesAndFlushAsync(path, bytes, cancellationToken);

        public bool MoveCreateNewIfIdentityMatches(
            string sourcePath,
            string destinationPath,
            string expectedSourceIdentity) =>
            _inner.MoveCreateNewIfIdentityMatches(
                sourcePath,
                destinationPath,
                expectedSourceIdentity);

        public bool ReplaceIfSourceIdentityMatches(
            string sourcePath,
            string destinationPath,
            string expectedSourceIdentity) =>
            _inner.ReplaceIfSourceIdentityMatches(
                sourcePath,
                destinationPath,
                expectedSourceIdentity);

        public bool DeleteIfIdentityMatches(string path, string expectedIdentity) =>
            _inner.DeleteIfIdentityMatches(path, expectedIdentity);
    }
}
