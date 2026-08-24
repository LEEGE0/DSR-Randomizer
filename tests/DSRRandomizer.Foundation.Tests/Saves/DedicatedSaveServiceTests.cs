using System.Security.Cryptography;
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

        Assert.True(result.Ready);
        Assert.True(result.ReusedExisting);
        Assert.DoesNotContain(
            fixture.Access.Opens,
            open => open.Path.EndsWith(".sl2", StringComparison.OrdinalIgnoreCase));
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
    public async Task ResetForSeedAsync_MetadataPublishFailure_RestoresPreviousSaveAndMetadata()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateNormalSave(fill: 0x41);
        fixture.CreateValidExternalRmm(fill: 0x22, seed: new SeedBinding("old-seed", "old-placement"));
        var priorSave = File.ReadAllBytes(fixture.DedicatedPath);
        var priorMetadata = File.ReadAllBytes(fixture.MetadataPath);
        fixture.Access.FailNextMetadataReplace = true;

        var result = await fixture.Service.ResetForSeedAsync(
            fixture.SteamId,
            new SeedBinding("new-seed", "new-placement"),
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
        fixture.CreateValidExternalRmm(fill: 0x22, seed: new SeedBinding("old-seed", "old-placement"));

        var result = await fixture.Service.ResetForSeedAsync(
            fixture.SteamId,
            new SeedBinding("new-seed", "new-placement"),
            default);

        Assert.True(result.Ready);
        Assert.Equal(fixture.NormalBytes, File.ReadAllBytes(fixture.DedicatedPath));
        var metadata = fixture.ReadMetadata();
        Assert.Equal("new-seed", metadata.ActiveSeedId);
        Assert.Equal("new-placement", metadata.PlacementSha256);
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
        fixture.CreateValidExternalRmm(fill: 0x22, seed: new SeedBinding("old-seed", "old-placement"));
        var priorSave = File.ReadAllBytes(fixture.DedicatedPath);
        var priorMetadata = File.ReadAllBytes(fixture.MetadataPath);
        fixture.Access.CorruptOnSecondHashPath = fixture.DedicatedPath;

        var result = await fixture.Service.ResetForSeedAsync(
            fixture.SteamId,
            new SeedBinding("new-seed", "new-placement"),
            default);

        Assert.False(result.Ready);
        Assert.Equal(priorSave, File.ReadAllBytes(fixture.DedicatedPath));
        Assert.Equal(priorMetadata, File.ReadAllBytes(fixture.MetadataPath));
    }

    [Fact]
    public async Task BeginSessionAsync_PersistsUncleanStateBeforeReturningReady()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateValidExternalRmm();

        var result = await fixture.Service.BeginSessionAsync(fixture.SteamId, default);

        Assert.True(result.Ready);
        Assert.False(fixture.ReadMetadata().CleanExit);
        Assert.True(fixture.Access.MetadataReplaceCompleted);
    }

    [Fact]
    public async Task CompleteSessionAsync_AbnormalExit_DoesNotUpdateHashOrMarkClean()
    {
        var fixture = await Fixture.CreateAsync(_root);
        fixture.CreateValidExternalRmm();
        await fixture.Service.BeginSessionAsync(fixture.SteamId, default);
        File.WriteAllBytes(fixture.DedicatedPath, fixture.Bytes(0x66));
        var before = fixture.ReadMetadata();

        var result = await fixture.Service.CompleteSessionAsync(
            fixture.SteamId,
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
        await fixture.Service.BeginSessionAsync(fixture.SteamId, default);
        var changed = fixture.Bytes(0x66);
        File.WriteAllBytes(fixture.DedicatedPath, changed);

        var result = await fixture.Service.CompleteSessionAsync(
            fixture.SteamId,
            normalGuardedExit: true,
            default);

        Assert.True(result.Ready);
        var metadata = fixture.ReadMetadata();
        Assert.True(metadata.CleanExit);
        Assert.Equal(Convert.ToHexString(SHA256.HashData(changed)).ToLowerInvariant(), metadata.LastKnownSha256);
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
        {
            Directory.CreateDirectory(SaveDirectory);
            var bytes = Bytes(fill, length);
            File.WriteAllBytes(DedicatedPath, bytes);
            WriteMetadata(new DedicatedSaveMetadata(
                1,
                SteamId,
                FixedSaveLength,
                lastKnownSha256 ?? Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant(),
                seed?.SeedId,
                seed?.PlacementSha256,
                cleanExit));
        }

        public DedicatedSaveMetadata ReadMetadata() =>
            JsonSerializer.Deserialize<DedicatedSaveMetadata>(
                File.ReadAllBytes(MetadataPath),
                JsonOptions)!;

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
        public bool MetadataReplaceCompleted { get; private set; }
        public string? CorruptOnSecondHashPath { get; set; }
        private Dictionary<string, int> HashPathCounts { get; } =
            new(StringComparer.OrdinalIgnoreCase);

        public bool Exists(string path) => inner.Exists(path);

        public void CreateDirectory(string path) => inner.CreateDirectory(path);

        public FileAttributes GetAttributes(string path) => inner.GetAttributes(path);

        public Stream Open(string path, FileMode mode, FileAccess access, FileShare share)
        {
            Opens.Add(new OpenObservation(Path.GetFullPath(path), mode, access, share));
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
            HashPathCounts.TryGetValue(path, out var count);
            count++;
            HashPathCounts[path] = count;
            if (count == 2
                && path.Equals(CorruptOnSecondHashPath, StringComparison.OrdinalIgnoreCase))
            {
                var bytes = File.ReadAllBytes(path);
                bytes[0] ^= 0xFF;
                File.WriteAllBytes(path, bytes);
            }

            return inner.IdentityAndHashAsync(path, cancellationToken);
        }

        public async Task CopyAndFlushAsync(
            Stream source,
            string destinationPath,
            CancellationToken cancellationToken)
        {
            if (CopyBehavior == CopyBehavior.Normal)
            {
                await inner.CopyAndFlushAsync(source, destinationPath, cancellationToken);
            }
            else
            {
                source.Position = 0;
                await using var destination = inner.Open(
                    destinationPath,
                    FileMode.CreateNew,
                    FileAccess.Write,
                    FileShare.None);
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

            AfterCopy?.Invoke(destinationPath);
        }

        public Task WriteAllBytesAndFlushAsync(
            string path,
            ReadOnlyMemory<byte> bytes,
            CancellationToken cancellationToken) =>
            inner.WriteAllBytesAndFlushAsync(path, bytes, cancellationToken);

        public void MoveCreateNew(string sourcePath, string destinationPath)
        {
            if (destinationPath.Equals(RaceDestinationPath, StringComparison.OrdinalIgnoreCase))
            {
                Directory.CreateDirectory(Path.GetDirectoryName(destinationPath)!);
                File.WriteAllBytes(destinationPath, RaceDestinationBytes!);
                throw new IOException("Injected destination race.");
            }

            inner.MoveCreateNew(sourcePath, destinationPath);
        }

        public void Replace(string sourcePath, string destinationPath)
        {
            if (FailNextMetadataReplace
                && destinationPath.EndsWith("save-metadata.json", StringComparison.OrdinalIgnoreCase))
            {
                FailNextMetadataReplace = false;
                throw new IOException("Injected metadata publish failure.");
            }

            inner.Replace(sourcePath, destinationPath);
            if (destinationPath.EndsWith("save-metadata.json", StringComparison.OrdinalIgnoreCase))
            {
                MetadataReplaceCompleted = true;
            }
        }

        public void Delete(string path) => inner.Delete(path);
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
}
