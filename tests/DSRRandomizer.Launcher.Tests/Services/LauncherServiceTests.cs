using System.Security.Cryptography;
using System.Text.Json;
using DSRRandomizer.Foundation.Saves;
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
        Assert.Equal(SaveErrorCode.MultipleProfilesRequireSelection, result.ErrorCode);
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

    public void Dispose()
    {
        if (Directory.Exists(_container))
        {
            Directory.Delete(_container, recursive: true);
        }
    }

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

        public FileAttributes GetAttributes(string path) => _inner.GetAttributes(path);

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

        public Task CopyAndFlushAsync(
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
