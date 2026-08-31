using System.Diagnostics;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.Json;

namespace DSRRandomizer.Launcher.Tests;

public sealed class PinnedArtifactPublishTests : IDisposable
{
    private const string Version = "0.1.0-alpha.2";
    private static readonly string[] ExpectedArchivePaths =
    [
        "CHANGELOG.md",
        "DSRForMod.Launcher.exe",
        "INSTALL_KO.md",
        "LICENSE",
        "README.md",
        "THIRD_PARTY_NOTICES.md",
        "components/rmm-bridge/DSRRandomizer.RmmBridge.dll",
        "components/rmm-bridge/DSRRandomizer.RmmBridgeHost.exe",
        "components/rmm-bridge/deployment-manifest.json",
        "config/compatibility-profiles.json",
        "native/DSRRandomizer.Runtime.dll",
        "native/DSRRandomizer.Runtime.dll.sha256"
    ];

    private readonly string _packageOutputRoot = Path.Combine(
        Path.GetTempPath(),
        $"dsr-official-release-{Guid.NewGuid():N}");

    [Fact]
    public async Task OfficialReleaseBuildPublishesPinnedPairAndCreatesValidatedExactArchive()
    {
        var repositoryRoot = FindRepositoryRoot();
        var firstBuild = await BuildReleaseAsync(repositoryRoot);
        Assert.True(
            firstBuild.ExitCode == 0,
            $"build-release.ps1 failed with exit code {firstBuild.ExitCode}.\n{firstBuild.Output}");

        var releaseWork = Path.Combine(
            repositoryRoot,
            "artifacts",
            $"release-work-{Version}");
        var launcherPublish = Path.Combine(releaseWork, "launcher");
        var hostPublish = Path.Combine(releaseWork, "rmm-bridge-host");
        var publishedBridge = Path.Combine(
            launcherPublish,
            "components",
            "rmm-bridge",
            "DSRRandomizer.RmmBridge.dll");
        var publishedHost = Path.Combine(
            launcherPublish,
            "components",
            "rmm-bridge",
            "DSRRandomizer.RmmBridgeHost.exe");
        var publishedManifest = Path.Combine(
            launcherPublish,
            "components",
            "rmm-bridge",
            "deployment-manifest.json");
        Assert.True(File.Exists(publishedBridge), $"Published bridge is missing: {publishedBridge}");
        Assert.True(File.Exists(publishedHost), $"Published host is missing: {publishedHost}");
        Assert.True(File.Exists(publishedManifest), $"Published manifest is missing: {publishedManifest}");

        var nativeBridge = Path.Combine(
            repositoryRoot,
            "native",
            "out",
            "build",
            "windows-x64-release",
            "native",
            "runtime",
            "Release",
            "DSRRandomizer.RmmBridge.dll");
        var selfContainedHost = Path.Combine(hostPublish, "DSRRandomizer.RmmBridgeHost.exe");
        Assert.Equal(Sha256(nativeBridge), Sha256(publishedBridge));
        Assert.Equal(Sha256(selfContainedHost), Sha256(publishedHost));
        AssertStrictManifest(publishedManifest, Sha256(publishedBridge), Sha256(publishedHost));

        await File.WriteAllTextAsync(publishedBridge, "future-dated tampered bridge");
        await File.WriteAllTextAsync(publishedHost, "future-dated tampered host");
        var future = DateTime.UtcNow.AddYears(1);
        File.SetLastWriteTimeUtc(publishedBridge, future);
        File.SetLastWriteTimeUtc(publishedHost, future);

        var secondBuild = await BuildReleaseAsync(repositoryRoot);
        Assert.True(
            secondBuild.ExitCode == 0,
            $"Second build-release.ps1 failed with exit code {secondBuild.ExitCode}.\n{secondBuild.Output}");
        Assert.Equal(Sha256(nativeBridge), Sha256(publishedBridge));
        Assert.Equal(Sha256(selfContainedHost), Sha256(publishedHost));
        AssertStrictManifest(publishedManifest, Sha256(publishedBridge), Sha256(publishedHost));

        var zipPath = Path.Combine(
            _packageOutputRoot,
            $"DSR-for-MOD-v{Version}-win-x64.zip");
        var checksumPath = $"{zipPath}.sha256";
        Assert.True(File.Exists(zipPath), $"Release ZIP is missing: {zipPath}");
        Assert.True(File.Exists(checksumPath), $"Release checksum is missing: {checksumPath}");

        using (var archive = ZipFile.OpenRead(zipPath))
        {
            var entries = archive.Entries.Select(entry => entry.FullName).ToArray();
            Assert.Equal(ExpectedArchivePaths, entries);
            Assert.DoesNotContain(entries, entry => entry.EndsWith(".pdb", StringComparison.OrdinalIgnoreCase));
            Assert.All(
                archive.Entries,
                entry => Assert.Equal(
                    new DateTime(1980, 1, 1, 0, 0, 0, DateTimeKind.Unspecified),
                    entry.LastWriteTime.DateTime));
        }

        var extractedRoot = Path.Combine(_packageOutputRoot, "independent-extract");
        ZipFile.ExtractToDirectory(zipPath, extractedRoot);
        var validation = await RunProcessAsync(
            Path.Combine(extractedRoot, "DSRForMod.Launcher.exe"),
            repositoryRoot,
            "--validate-package",
            extractedRoot);
        Assert.True(
            validation.ExitCode == 0,
            $"Extracted package validation failed with exit code {validation.ExitCode}.\n{validation.Output}");

        var zipHash = Sha256(zipPath).ToLowerInvariant();
        Assert.Equal(
            $"{zipHash}  {Path.GetFileName(zipPath)}\n",
            (await File.ReadAllTextAsync(checksumPath)).Replace("\r\n", "\n", StringComparison.Ordinal));
    }

    [Theory]
    [InlineData("duplicate")]
    [InlineData("rooted")]
    [InlineData("traversal")]
    public async Task ArchiveValidatorRejectsUnsafeEntryNames(string caseName)
    {
        Directory.CreateDirectory(_packageOutputRoot);
        var archivePath = Path.Combine(_packageOutputRoot, $"unsafe-{caseName}.zip");
        using (var archive = ZipFile.Open(archivePath, ZipArchiveMode.Create))
        {
            switch (caseName)
            {
                case "duplicate":
                    archive.CreateEntry("README.md");
                    archive.CreateEntry("README.md");
                    break;
                case "rooted":
                    archive.CreateEntry("/README.md");
                    break;
                case "traversal":
                    archive.CreateEntry("../README.md");
                    break;
                default:
                    throw new ArgumentOutOfRangeException(nameof(caseName), caseName, null);
            }
        }

        var repositoryRoot = FindRepositoryRoot();
        var result = await RunProcessAsync(
            "pwsh.exe",
            repositoryRoot,
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            Path.Combine(repositoryRoot, "packaging", "package.ps1"),
            "-ValidateArchivePath",
            archivePath);

        Assert.NotEqual(0, result.ExitCode);
        Assert.Contains("Release archive entry validation failed", result.Output, StringComparison.Ordinal);
    }

    private Task<(int ExitCode, string Output)> BuildReleaseAsync(string repositoryRoot) =>
        RunProcessAsync(
            "pwsh.exe",
            repositoryRoot,
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            Path.Combine(repositoryRoot, "packaging", "build-release.ps1"),
            "-Version",
            Version,
            "-OutputPath",
            _packageOutputRoot);

    private static async Task<(int ExitCode, string Output)> RunProcessAsync(
        string fileName,
        string workingDirectory,
        params string[] arguments)
    {
        var startInfo = new ProcessStartInfo(fileName)
        {
            WorkingDirectory = workingDirectory,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true
        };
        foreach (var argument in arguments)
        {
            startInfo.ArgumentList.Add(argument);
        }

        using var process = Process.Start(startInfo)
            ?? throw new InvalidOperationException($"Unable to start {fileName}.");
        var outputTask = process.StandardOutput.ReadToEndAsync();
        var errorTask = process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync();
        return (process.ExitCode, $"{await outputTask}\n{await errorTask}");
    }

    private static void AssertStrictManifest(
        string path,
        string expectedBridgeHash,
        string expectedHostHash)
    {
        using var document = JsonDocument.Parse(File.ReadAllBytes(path));
        var properties = document.RootElement.EnumerateObject().ToArray();
        Assert.Equal(
            ["schemaVersion", "configuration", "bridgeSha256", "hostSha256"],
            properties.Select(property => property.Name));
        Assert.Equal(1, document.RootElement.GetProperty("schemaVersion").GetInt32());
        Assert.Equal("Release", document.RootElement.GetProperty("configuration").GetString());
        Assert.Equal(
            expectedBridgeHash.ToLowerInvariant(),
            document.RootElement.GetProperty("bridgeSha256").GetString());
        Assert.Equal(
            expectedHostHash.ToLowerInvariant(),
            document.RootElement.GetProperty("hostSha256").GetString());
    }

    private static string Sha256(string path) => Convert.ToHexString(
        SHA256.HashData(File.ReadAllBytes(path)));

    private static string FindRepositoryRoot()
    {
        var current = new DirectoryInfo(AppContext.BaseDirectory);
        while (current is not null)
        {
            if (File.Exists(Path.Combine(current.FullName, ".git"))
                || Directory.Exists(Path.Combine(current.FullName, ".git")))
            {
                return current.FullName;
            }
            current = current.Parent;
        }
        throw new DirectoryNotFoundException("Unable to locate the repository root.");
    }

    public void Dispose()
    {
        if (Directory.Exists(_packageOutputRoot))
        {
            Directory.Delete(_packageOutputRoot, recursive: true);
        }
    }
}
