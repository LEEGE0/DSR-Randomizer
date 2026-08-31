using System.Diagnostics;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
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
    public async Task ReleaseHostBundleExcludesDrSwizzlerAssemblyAndReferences()
    {
        var repositoryRoot = FindRepositoryRoot();
        var publishRoot = Path.Combine(_packageOutputRoot, "bridge-host-publish");
        var publish = await RunProcessAsync(
            "dotnet.exe",
            repositoryRoot,
            "publish",
            Path.Combine(
                repositoryRoot,
                "src",
                "DSRRandomizer.RmmBridgeHost",
                "DSRRandomizer.RmmBridgeHost.csproj"),
            "-c",
            "Release",
            "--no-restore",
            "-nr:false",
            "-o",
            publishRoot);
        Assert.True(
            publish.ExitCode == 0,
            $"Bridge-host publish failed with exit code {publish.ExitCode}.\n{publish.Output}");

        var hostPath = Path.Combine(publishRoot, "DSRRandomizer.RmmBridgeHost.exe");
        Assert.True(File.Exists(hostPath), $"Published bridge host is missing: {hostPath}");

        var bundle = ReadSingleFileBundle(hostPath);
        Assert.Equal(6u, bundle.MajorVersion);
        Assert.DoesNotContain(
            bundle.Entries,
            entry => string.Equals(entry.RelativePath, "DrSwizzler.dll", StringComparison.OrdinalIgnoreCase));
        Assert.DoesNotContain(
            bundle.Entries,
            entry => string.Equals(
                entry.RelativePath,
                "BouncyCastle.Cryptography.dll",
                StringComparison.OrdinalIgnoreCase));

        var depsEntry = Assert.Single(bundle.Entries, entry => entry.Type == 3);
        var depsJson = Encoding.UTF8.GetString(ReadBundleEntry(hostPath, depsEntry));
        Assert.DoesNotContain("DrSwizzler", depsJson, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("BouncyCastle", depsJson, StringComparison.OrdinalIgnoreCase);

        var soulsFormatsEntry = Assert.Single(
            bundle.Entries,
            entry => string.Equals(entry.RelativePath, "SoulsFormats.dll", StringComparison.Ordinal));
        AssertBytesDoNotContainEncoded(
            ReadBundleEntry(hostPath, soulsFormatsEntry),
            Environment.GetFolderPath(Environment.SpecialFolder.UserProfile));

        var subsetProject = Path.Combine(
            repositoryRoot,
            "src",
            "DSRRandomizer.SoulsFormatsSubset",
            "DSRRandomizer.SoulsFormatsSubset.csproj");
        var subsetReleaseRoot = Path.Combine(_packageOutputRoot, "subset-release");
        var subsetDebugRoot = Path.Combine(_packageOutputRoot, "subset-debug");
        foreach (var (configuration, output) in new[]
                 {
                     ("Release", subsetReleaseRoot),
                     ("Debug", subsetDebugRoot)
                 })
        {
            var build = await RunProcessAsync(
                "dotnet.exe",
                repositoryRoot,
                "build",
                subsetProject,
                "-c",
                configuration,
                "--no-restore",
                "-nr:false",
                "-o",
                output);
            Assert.True(
                build.ExitCode == 0,
                $"Subset {configuration} build failed with exit code {build.ExitCode}.\n{build.Output}");
        }

        Assert.False(File.Exists(Path.Combine(subsetReleaseRoot, "SoulsFormats.pdb")));
        Assert.True(File.Exists(Path.Combine(subsetDebugRoot, "SoulsFormats.pdb")));
    }

    [Fact]
    public async Task OfficialSourceReleaseBuildCreatesDeterministicCommittedTreeArchive()
    {
        var repositoryRoot = FindRepositoryRoot();
        var firstOutput = Path.Combine(_packageOutputRoot, "source-first");
        var secondOutput = Path.Combine(_packageOutputRoot, "source-second");

        foreach (var output in new[] { firstOutput, secondOutput })
        {
            var build = await RunProcessAsync(
                "pwsh.exe",
                repositoryRoot,
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                Path.Combine(repositoryRoot, "packaging", "build-source-release.ps1"),
                "-Version",
                Version,
                "-OutputPath",
                output);
            Assert.True(
                build.ExitCode == 0,
                $"build-source-release.ps1 failed with exit code {build.ExitCode}.\n{build.Output}");
        }

        var archiveName = $"DSR-for-MOD-v{Version}-source.zip";
        var firstArchive = Path.Combine(firstOutput, archiveName);
        var secondArchive = Path.Combine(secondOutput, archiveName);
        Assert.True(File.Exists(firstArchive), $"Source archive is missing: {firstArchive}");
        Assert.Equal(Sha256(firstArchive), Sha256(secondArchive));

        var prefix = $"DSR-for-MOD-v{Version}-source/";
        using (var archive = ZipFile.OpenRead(firstArchive))
        {
            var entries = archive.Entries.Select(entry => entry.FullName).ToArray();
            Assert.NotEmpty(entries);
            Assert.Equal(entries.Order(StringComparer.Ordinal), entries);
            Assert.Equal(entries.Length, entries.Distinct(StringComparer.Ordinal).Count());
            Assert.All(entries, entry => Assert.StartsWith(prefix, entry, StringComparison.Ordinal));
            Assert.Contains($"{prefix}DSR-Randomizer.sln", entries);
            Assert.Contains(
                $"{prefix}src/DSRRandomizer.SoulsFormatsSubset/DSRRandomizer.SoulsFormatsSubset.csproj",
                entries);
            Assert.Contains($"{prefix}third_party/SoulsFormatsNEXT/LICENSE", entries);
            Assert.Contains(
                $"{prefix}third_party/SoulsFormatsNEXT/SoulsFormats/Formats/Binder/BND3/BND3.cs",
                entries);
            Assert.DoesNotContain(entries, IsProhibitedSourceArchivePath);
            Assert.All(
                archive.Entries,
                entry => Assert.Equal(
                    new DateTime(1980, 1, 1, 0, 0, 0, DateTimeKind.Unspecified),
                    entry.LastWriteTime.DateTime));
        }

        AssertChecksumMatches(firstArchive);
        AssertChecksumMatches(secondArchive);
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

    private static bool IsProhibitedSourceArchivePath(string path)
    {
        var segments = path.Split('/', StringSplitOptions.RemoveEmptyEntries);
        return segments.Any(segment => segment.Equals(".git", StringComparison.OrdinalIgnoreCase)
            || segment.Equals("bin", StringComparison.OrdinalIgnoreCase)
            || segment.Equals("obj", StringComparison.OrdinalIgnoreCase)
            || segment.Equals("artifacts", StringComparison.OrdinalIgnoreCase)
            || segment.Equals("private", StringComparison.OrdinalIgnoreCase)
            || segment.Equals("generated", StringComparison.OrdinalIgnoreCase));
    }

    private static void AssertChecksumMatches(string archivePath)
    {
        var expected = $"{Sha256(archivePath).ToLowerInvariant()}  {Path.GetFileName(archivePath)}\n";
        var actual = File.ReadAllText($"{archivePath}.sha256")
            .Replace("\r\n", "\n", StringComparison.Ordinal);
        Assert.Equal(expected, actual);
    }

    private static void AssertBytesDoNotContainEncoded(byte[] bytes, string value)
    {
        foreach (var encoding in new[] { Encoding.UTF8, Encoding.Unicode, Encoding.BigEndianUnicode })
        {
            Assert.True(
                bytes.AsSpan().IndexOf(encoding.GetBytes(value)) < 0,
                $"Published binary unexpectedly contains local profile marker using {encoding.WebName}.");
        }
    }

    private static SingleFileBundle ReadSingleFileBundle(string path)
    {
        // This is Bundler.BundleHeaderSignature from Microsoft.NET.HostModel 8.0.
        // BinaryUtils performs the same memory-mapped KMP search used by HostModel;
        // the Int64 immediately before the signature is the manifest header offset.
        byte[] signature =
        [
            0x8b, 0x12, 0x02, 0xb9, 0x6a, 0x61, 0x20, 0x38,
            0x72, 0x7b, 0x93, 0x02, 0x14, 0xd7, 0xa0, 0x32,
            0x13, 0xf5, 0xb9, 0xe6, 0xef, 0xae, 0x33, 0x18,
            0xee, 0x3b, 0x2d, 0xce, 0x24, 0xb3, 0x6a, 0xae,
        ];
        var signatureOffset = Microsoft.NET.HostModel.AppHost.BinaryUtils.SearchInFile(path, signature);
        if (signatureOffset < sizeof(long))
        {
            throw new InvalidDataException("The .NET single-file bundle signature was not found.");
        }
        using var stream = File.OpenRead(path);
        stream.Position = signatureOffset - sizeof(long);
        using var offsetReader = new BinaryReader(stream, Encoding.UTF8, leaveOpen: true);
        var headerOffset = offsetReader.ReadInt64();
        if (headerOffset <= 0 || headerOffset >= stream.Length)
        {
            throw new InvalidDataException($"Invalid single-file bundle header offset: {headerOffset}.");
        }

        using var reader = new BinaryReader(stream, Encoding.UTF8, leaveOpen: false);
        stream.Position = headerOffset;

        var majorVersion = reader.ReadUInt32();
        _ = reader.ReadUInt32(); // Minor version is currently zero.
        var fileCount = reader.ReadInt32();
        if (fileCount < 0)
        {
            throw new InvalidDataException($"Invalid single-file bundle entry count: {fileCount}.");
        }

        _ = reader.ReadString(); // Bundle ID.
        if (majorVersion >= 2)
        {
            _ = reader.ReadInt64(); // Deps JSON offset.
            _ = reader.ReadInt64(); // Deps JSON size.
            _ = reader.ReadInt64(); // Runtimeconfig JSON offset.
            _ = reader.ReadInt64(); // Runtimeconfig JSON size.
            _ = reader.ReadUInt64(); // Header flags.
        }

        var entries = new List<SingleFileBundleEntry>(fileCount);
        for (var index = 0; index < fileCount; index++)
        {
            var offset = reader.ReadInt64();
            var size = reader.ReadInt64();
            var compressedSize = majorVersion >= 6 ? reader.ReadInt64() : 0;
            var type = reader.ReadByte();
            var relativePath = reader.ReadString();
            entries.Add(new SingleFileBundleEntry(offset, size, compressedSize, type, relativePath));
        }

        return new SingleFileBundle(majorVersion, entries);
    }

    private static byte[] ReadBundleEntry(string bundlePath, SingleFileBundleEntry entry)
    {
        using var bundle = File.OpenRead(bundlePath);
        bundle.Position = entry.Offset;

        var storedSize = entry.CompressedSize > 0 ? entry.CompressedSize : entry.Size;
        if (storedSize < 0 || storedSize > int.MaxValue || entry.Offset + storedSize > bundle.Length)
        {
            throw new InvalidDataException(
                $"Invalid bundle entry range for '{entry.RelativePath}': {entry.Offset}+{storedSize}.");
        }

        var stored = new byte[(int)storedSize];
        bundle.ReadExactly(stored);
        if (entry.CompressedSize == 0)
        {
            return stored;
        }

        using var input = new MemoryStream(stored, writable: false);
        using var deflate = new DeflateStream(input, CompressionMode.Decompress);
        using var output = new MemoryStream(entry.Size <= int.MaxValue ? (int)entry.Size : 0);
        deflate.CopyTo(output);
        if (output.Length != entry.Size)
        {
            throw new InvalidDataException(
                $"Decompressed bundle entry '{entry.RelativePath}' has size {output.Length}, expected {entry.Size}.");
        }

        return output.ToArray();
    }

    private sealed record SingleFileBundle(uint MajorVersion, IReadOnlyList<SingleFileBundleEntry> Entries);

    private sealed record SingleFileBundleEntry(
        long Offset,
        long Size,
        long CompressedSize,
        byte Type,
        string RelativePath);

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
