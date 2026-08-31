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
        var existingWorkDirectories = ReleaseWorkDirectories(repositoryRoot);
        var build = await BuildReleaseAsync(repositoryRoot);
        Assert.True(
            build.ExitCode == 0,
            $"build-release.ps1 failed with exit code {build.ExitCode}.\n{build.Output}");
        Assert.Equal(existingWorkDirectories, ReleaseWorkDirectories(repositoryRoot));

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

        var extractedBridge = Path.Combine(
            extractedRoot,
            "components",
            "rmm-bridge",
            "DSRRandomizer.RmmBridge.dll");
        var extractedHost = Path.Combine(
            extractedRoot,
            "components",
            "rmm-bridge",
            "DSRRandomizer.RmmBridgeHost.exe");
        AssertStrictManifest(
            Path.Combine(
                extractedRoot,
                "components",
                "rmm-bridge",
                "deployment-manifest.json"),
            Sha256(extractedBridge),
            Sha256(extractedHost));
        Assert.Equal(
            Sha256(Path.Combine(
                repositoryRoot,
                "native",
                "out",
                "build",
                "windows-x64-release",
                "native",
                "runtime",
                "Release",
                "DSRRandomizer.RmmBridge.dll")),
            Sha256(extractedBridge));

        var zipHash = Sha256(zipPath).ToLowerInvariant();
        Assert.Equal(
            $"{zipHash}  {Path.GetFileName(zipPath)}\n",
            (await File.ReadAllTextAsync(checksumPath)).Replace("\r\n", "\n", StringComparison.Ordinal));
    }

    [Fact]
    public async Task OfficialReleaseBuildCleansWorkDirectoryAndPreservesOutputsOnControlledFailure()
    {
        var repositoryRoot = FindRepositoryRoot();
        Directory.CreateDirectory(_packageOutputRoot);
        var zipPath = Path.Combine(
            _packageOutputRoot,
            $"DSR-for-MOD-v{Version}-win-x64.zip");
        var checksumPath = $"{zipPath}.sha256";
        var originalZip = new byte[] { 1, 3, 3, 7 };
        var originalChecksum = new byte[] { 4, 2 };
        await File.WriteAllBytesAsync(zipPath, originalZip);
        await File.WriteAllBytesAsync(checksumPath, originalChecksum);
        var existingWorkDirectories = ReleaseWorkDirectories(repositoryRoot);

        var build = await BuildReleaseAsync(
            repositoryRoot,
            new Dictionary<string, string>
            {
                ["MSBuildSDKsPath"] = Path.Combine(_packageOutputRoot, "missing-msbuild-sdks")
            });

        Assert.NotEqual(0, build.ExitCode);
        Assert.Equal(existingWorkDirectories, ReleaseWorkDirectories(repositoryRoot));
        Assert.Contains("Native Release build or tests failed", build.Output, StringComparison.Ordinal);
        Assert.Equal(originalZip, await File.ReadAllBytesAsync(zipPath));
        Assert.Equal(originalChecksum, await File.ReadAllBytesAsync(checksumPath));
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

    [Fact]
    public async Task SafeReleaseDirectoryScriptRejectsJunctionsAndPreservesOutsideSentinel()
    {
        var repositoryRoot = FindRepositoryRoot();
        var result = await RunProcessAsync(
            "pwsh.exe",
            repositoryRoot,
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            Path.Combine(
                repositoryRoot,
                "packaging",
                "tests",
                "Test-SafeReleaseDirectories.ps1"));

        Assert.True(
            result.ExitCode == 0,
            $"Safe release-directory test failed with exit code {result.ExitCode}.\n{result.Output}");
    }

    private Task<(int ExitCode, string Output)> BuildReleaseAsync(
        string repositoryRoot,
        IReadOnlyDictionary<string, string>? environment = null) =>
        RunProcessAsync(
            "pwsh.exe",
            repositoryRoot,
            environment,
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            Path.Combine(repositoryRoot, "packaging", "build-release.ps1"),
            "-Version",
            Version,
            "-OutputPath",
            _packageOutputRoot);

    private static HashSet<string> ReleaseWorkDirectories(string repositoryRoot)
    {
        var artifactsRoot = Path.Combine(repositoryRoot, "artifacts");
        return Directory.Exists(artifactsRoot)
            ? Directory.EnumerateDirectories(
                    artifactsRoot,
                    $"release-work-{Version}-*",
                    SearchOption.TopDirectoryOnly)
                .Select(Path.GetFullPath)
                .ToHashSet(StringComparer.OrdinalIgnoreCase)
            : new HashSet<string>(StringComparer.OrdinalIgnoreCase);
    }

    private static async Task<(int ExitCode, string Output)> RunProcessAsync(
        string fileName,
        string workingDirectory,
        params string[] arguments) =>
        await RunProcessAsync(fileName, workingDirectory, environment: null, arguments);

    private static async Task<(int ExitCode, string Output)> RunProcessAsync(
        string fileName,
        string workingDirectory,
        IReadOnlyDictionary<string, string>? environment,
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
        if (environment is not null)
        {
            foreach (var (name, value) in environment)
            {
                startInfo.Environment[name] = value;
            }
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
