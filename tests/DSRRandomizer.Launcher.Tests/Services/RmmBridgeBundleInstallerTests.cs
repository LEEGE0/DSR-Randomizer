using System.Diagnostics;
using System.Text;
using DSRRandomizer.Launcher.Safety;
using DSRRandomizer.Launcher.Services;

namespace DSRRandomizer.Launcher.Tests.Services;

public sealed class RmmBridgeBundleInstallerTests
{
    private static readonly byte[] BridgeBytes = Encoding.UTF8.GetBytes("bridge-v1");
    private static readonly byte[] HostBytes = Encoding.UTF8.GetBytes("host-v1");
    private static readonly byte[] StaleBridgeBytes = Encoding.UTF8.GetBytes("stale-bridge");
    private static readonly byte[] StaleHostBytes = Encoding.UTF8.GetBytes("stale-host");

    private const string BridgeSha256 =
        "83a3608e5baeb253b1670222090007d078fc84ef96fc6ce51e49de40986a332c";
    private const string HostSha256 =
        "7c133d47e5516e814f0d8a33976cc23237ac1aebae37bb6c9fd4f20ae8ba5b5e";
    private const string BundleInvalid = "RMM_BRIDGE_BUNDLE_INVALID";
    private const string InstallFailed = "RMM_BRIDGE_INSTALL_FAILED";
    private const string InstallTampered = "RMM_BRIDGE_INSTALL_TAMPERED";

    [Fact]
    public void EnsureInstalled_InstallsPinnedBundleIntoFreshExternalRoot()
    {
        using var fixture = Fixture.Create();

        var result = fixture.Installer.EnsureInstalled(fixture.ExternalRoot);

        Assert.Equal(RmmBridgeInstallResult.Ready(changed: true), result);
        Assert.Equal(BridgeBytes, File.ReadAllBytes(fixture.InstalledBridgePath));
        Assert.Equal(HostBytes, File.ReadAllBytes(fixture.InstalledHostPath));
        Assert.Equal(fixture.ManifestBytes, File.ReadAllBytes(fixture.InstalledManifestPath));
        Assert.Equal(new[] { "components" }, fixture.TopLevelEntries());
    }

    [Fact]
    public void EnsureInstalled_DoesNotReplaceMatchingBundle()
    {
        using var fixture = Fixture.Create();
        Assert.True(fixture.Installer.EnsureInstalled(fixture.ExternalRoot).IsReady);
        var timestamp = new DateTime(2020, 1, 2, 3, 4, 6, DateTimeKind.Utc);
        File.SetLastWriteTimeUtc(fixture.InstalledBridgePath, timestamp);
        File.SetLastWriteTimeUtc(fixture.InstalledHostPath, timestamp);
        File.SetLastWriteTimeUtc(fixture.InstalledManifestPath, timestamp);

        var result = fixture.Installer.EnsureInstalled(fixture.ExternalRoot);

        Assert.Equal(RmmBridgeInstallResult.Ready(changed: false), result);
        Assert.Equal(timestamp, File.GetLastWriteTimeUtc(fixture.InstalledBridgePath));
        Assert.Equal(timestamp, File.GetLastWriteTimeUtc(fixture.InstalledHostPath));
        Assert.Equal(timestamp, File.GetLastWriteTimeUtc(fixture.InstalledManifestPath));
    }

    [Fact]
    public void EnsureInstalled_ReplacesAStalePair()
    {
        using var fixture = Fixture.Create();
        fixture.WriteInstalledBundle(StaleBridgeBytes, StaleHostBytes, Encoding.UTF8.GetBytes("{}"));

        var result = fixture.Installer.EnsureInstalled(fixture.ExternalRoot);

        Assert.Equal(RmmBridgeInstallResult.Ready(changed: true), result);
        Assert.Equal(BridgeBytes, File.ReadAllBytes(fixture.InstalledBridgePath));
        Assert.Equal(HostBytes, File.ReadAllBytes(fixture.InstalledHostPath));
        Assert.Equal(fixture.ManifestBytes, File.ReadAllBytes(fixture.InstalledManifestPath));
    }

    [Fact]
    public void EnsureInstalled_RejectsAMissingPackagedSource()
    {
        using var fixture = Fixture.Create();
        File.Delete(fixture.PackagedHostPath);

        var result = fixture.Installer.EnsureInstalled(fixture.ExternalRoot);

        Assert.Equal(RmmBridgeInstallResult.Failed(BundleInvalid), result);
        Assert.False(Directory.Exists(fixture.InstalledBundleRoot));
    }

    [Fact]
    public void EnsureInstalled_RejectsAPackagedSourceHashMismatch()
    {
        using var fixture = Fixture.Create();
        File.WriteAllBytes(fixture.PackagedBridgePath, Encoding.UTF8.GetBytes("bridge-v2"));

        var result = fixture.Installer.EnsureInstalled(fixture.ExternalRoot);

        Assert.Equal(RmmBridgeInstallResult.Failed(BundleInvalid), result);
        Assert.False(Directory.Exists(fixture.InstalledBundleRoot));
    }

    [Fact]
    public void EnsureInstalled_RejectsAMalformedManifest()
    {
        using var fixture = Fixture.Create();
        File.WriteAllText(fixture.PackagedManifestPath, "{not-json", new UTF8Encoding(false));

        var result = fixture.Installer.EnsureInstalled(fixture.ExternalRoot);

        Assert.Equal(RmmBridgeInstallResult.Failed(BundleInvalid), result);
        Assert.False(Directory.Exists(fixture.InstalledBundleRoot));
    }

    [Fact]
    public void EnsureInstalled_RejectsAnExactFourPropertyManifestWithTheWrongHostHash()
    {
        using var fixture = Fixture.Create();
        File.WriteAllText(
            fixture.PackagedManifestPath,
            $$"""
              {"schemaVersion":1,"configuration":"Release","bridgeSha256":"{{BridgeSha256}}","hostSha256":"{{new string('0', 64)}}"}
              """,
            new UTF8Encoding(false));

        var result = fixture.Installer.EnsureInstalled(fixture.ExternalRoot);

        Assert.Equal(RmmBridgeInstallResult.Failed(BundleInvalid), result);
        Assert.False(Directory.Exists(fixture.InstalledBundleRoot));
    }

    [Fact]
    public void EnsureInstalled_RejectsACorrectHashManifestWithAnExtraProperty()
    {
        using var fixture = Fixture.Create();
        File.WriteAllText(
            fixture.PackagedManifestPath,
            $$"""
              {"schemaVersion":1,"configuration":"Release","bridgeSha256":"{{BridgeSha256}}","hostSha256":"{{HostSha256}}","extra":true}
              """,
            new UTF8Encoding(false));

        var result = fixture.Installer.EnsureInstalled(fixture.ExternalRoot);

        Assert.Equal(RmmBridgeInstallResult.Failed(BundleInvalid), result);
        Assert.False(Directory.Exists(fixture.InstalledBundleRoot));
    }

    [Fact]
    public void EnsureInstalled_RejectsADestinationReparsePoint()
    {
        using var fixture = Fixture.Create();
        var outside = Path.Combine(fixture.Container, "outside");
        Directory.CreateDirectory(outside);
        Directory.CreateDirectory(Path.Combine(fixture.ExternalRoot, "components"));
        fixture.CreateJunction(fixture.InstalledBundleRoot, outside);

        var result = fixture.Installer.EnsureInstalled(fixture.ExternalRoot);

        Assert.Equal(RmmBridgeInstallResult.Failed(InstallFailed), result);
        Assert.Empty(Directory.EnumerateFileSystemEntries(outside));
    }

    [Fact]
    public void EnsureInstalled_RejectsAnExternalRootPathThatEscapesThroughAParentSegment()
    {
        using var fixture = Fixture.Create();
        var selectedRoot = Path.Combine(fixture.Container, "selected");
        var escapedRoot = Path.Combine(fixture.Container, "escaped");
        Directory.CreateDirectory(selectedRoot);
        Directory.CreateDirectory(escapedRoot);
        var escapingPath = Path.Combine(selectedRoot, "..", "escaped");

        var result = fixture.Installer.EnsureInstalled(escapingPath);

        Assert.Equal(RmmBridgeInstallResult.Failed(InstallFailed), result);
        Assert.Empty(Directory.EnumerateFileSystemEntries(escapedRoot));
    }

    [Fact]
    public async Task EnsureInstalled_HeldDestinationAncestorCannotBeRenamedAndSwapped()
    {
        using var fixture = Fixture.Create();
        using var destinationLocked = new ManualResetEventSlim();
        using var continueInstall = new ManualResetEventSlim();
        var installer = fixture.CreateInstaller(
            onDestinationLocked: () =>
            {
                destinationLocked.Set();
                Assert.True(continueInstall.Wait(TimeSpan.FromSeconds(10)));
            });

        var install = Task.Run(() => installer.EnsureInstalled(fixture.ExternalRoot));
        Assert.True(destinationLocked.Wait(TimeSpan.FromSeconds(10)));
        var components = Path.Combine(fixture.ExternalRoot, "components");
        var displaced = Path.Combine(fixture.ExternalRoot, "components-displaced");
        try
        {
            Assert.Throws<IOException>(() => Directory.Move(components, displaced));
            Assert.True(Directory.Exists(components));
            Assert.False(Directory.Exists(displaced));
        }
        finally
        {
            continueInstall.Set();
        }

        Assert.Equal(RmmBridgeInstallResult.Ready(changed: true), await install);
    }

    [Fact]
    public void EnsureInstalled_RepairsAnInterruptedMixedPairOnTheNextCall()
    {
        using var fixture = Fixture.Create();
        fixture.WriteInstalledBundle(StaleBridgeBytes, StaleHostBytes, Encoding.UTF8.GetBytes("{}"));
        using (FileStream lockHost = new(
                   fixture.InstalledHostPath,
                   FileMode.Open,
                   FileAccess.Read,
                   FileShare.Read))
        {
            var interrupted = fixture.Installer.EnsureInstalled(fixture.ExternalRoot);

            Assert.Equal(RmmBridgeInstallResult.Failed(InstallFailed), interrupted);
            Assert.Equal(BridgeBytes, File.ReadAllBytes(fixture.InstalledBridgePath));
            Assert.Equal(StaleHostBytes, File.ReadAllBytes(fixture.InstalledHostPath));
            Assert.Equal("{}", File.ReadAllText(fixture.InstalledManifestPath));
        }

        var repaired = fixture.Installer.EnsureInstalled(fixture.ExternalRoot);

        Assert.Equal(RmmBridgeInstallResult.Ready(changed: true), repaired);
        Assert.Equal(BridgeBytes, File.ReadAllBytes(fixture.InstalledBridgePath));
        Assert.Equal(HostBytes, File.ReadAllBytes(fixture.InstalledHostPath));
        Assert.Equal(fixture.ManifestBytes, File.ReadAllBytes(fixture.InstalledManifestPath));
    }

    [Fact]
    public void EnsureInstalled_PreservesPackageOwnedContent()
    {
        using var fixture = Fixture.Create();
        var contentPath = Path.Combine(
            fixture.InstalledBundleRoot,
            "content",
            "overhaul",
            "GameParam.parambnd.dcx");
        Directory.CreateDirectory(Path.GetDirectoryName(contentPath)!);
        byte[] content = [13, 21, 34, 55, 89];
        File.WriteAllBytes(contentPath, content);

        var result = fixture.Installer.EnsureInstalled(fixture.ExternalRoot);

        Assert.True(result.IsReady);
        Assert.Equal(content, File.ReadAllBytes(contentPath));
    }

    [Fact]
    public void EnsureInstalled_MapsExpectedFinalVerificationFailureToTampered()
    {
        using var fixture = Fixture.Create();
        var verificationReached = false;
        var installer = fixture.CreateInstaller(
            beforeFinalVerification: () =>
            {
                verificationReached = true;
                throw new IOException("Deterministic final verification failure.");
            });

        var result = installer.EnsureInstalled(fixture.ExternalRoot);

        Assert.True(verificationReached);
        Assert.Equal(RmmBridgeInstallResult.Failed(InstallTampered), result);
        Assert.Equal(BridgeBytes, File.ReadAllBytes(fixture.InstalledBridgePath));
        Assert.Equal(HostBytes, File.ReadAllBytes(fixture.InstalledHostPath));
        Assert.Equal(fixture.ManifestBytes, File.ReadAllBytes(fixture.InstalledManifestPath));
    }

    private sealed class Fixture : IDisposable
    {
        private readonly List<string> _junctions = [];

        private Fixture(string container)
        {
            Container = container;
            PackageRoot = Path.Combine(container, "package");
            ExternalRoot = Path.Combine(container, "external");
            Directory.CreateDirectory(PackagedBundleRoot);
            Directory.CreateDirectory(ExternalRoot);
            File.WriteAllBytes(PackagedBridgePath, BridgeBytes);
            File.WriteAllBytes(PackagedHostPath, HostBytes);
            File.WriteAllBytes(PackagedManifestPath, ManifestBytes);
            Identities = new LaunchArtifactIdentities(
                new string('0', 64),
                new string('1', 64),
                BridgeSha256,
                HostSha256);
            Installer = CreateInstaller();
        }

        public string Container { get; }
        public string PackageRoot { get; }
        public string ExternalRoot { get; }
        public LaunchArtifactIdentities Identities { get; }
        public RmmBridgeBundleInstaller Installer { get; }
        public string PackagedBundleRoot => Path.Combine(PackageRoot, "components", "rmm-bridge");
        public string PackagedBridgePath => Path.Combine(PackagedBundleRoot, "DSRRandomizer.RmmBridge.dll");
        public string PackagedHostPath => Path.Combine(PackagedBundleRoot, "DSRRandomizer.RmmBridgeHost.exe");
        public string PackagedManifestPath => Path.Combine(PackagedBundleRoot, "deployment-manifest.json");
        public string InstalledBundleRoot => Path.Combine(ExternalRoot, "components", "rmm-bridge");
        public string InstalledBridgePath => Path.Combine(InstalledBundleRoot, "DSRRandomizer.RmmBridge.dll");
        public string InstalledHostPath => Path.Combine(InstalledBundleRoot, "DSRRandomizer.RmmBridgeHost.exe");
        public string InstalledManifestPath => Path.Combine(InstalledBundleRoot, "deployment-manifest.json");
        public byte[] ManifestBytes => Encoding.UTF8.GetBytes(
            $$"""
              {"schemaVersion":1,"configuration":"Release","bridgeSha256":"{{BridgeSha256}}","hostSha256":"{{HostSha256}}"}
              """);

        public static Fixture Create()
        {
            var container = Path.Combine(Path.GetTempPath(), $"dsr-rmm-installer-{Guid.NewGuid():N}");
            Directory.CreateDirectory(container);
            return new Fixture(container);
        }

        public RmmBridgeBundleInstaller CreateInstaller(
            Action? onDestinationLocked = null,
            Action? beforeFinalVerification = null) => new(
                PackageRoot,
                Identities,
                onDestinationLocked,
                beforeFinalVerification);

        public string[] TopLevelEntries() => Directory
            .EnumerateFileSystemEntries(ExternalRoot)
            .Select(Path.GetFileName)
            .Order(StringComparer.Ordinal)
            .ToArray()!;

        public void WriteInstalledBundle(byte[] bridge, byte[] host, byte[] manifest)
        {
            Directory.CreateDirectory(InstalledBundleRoot);
            File.WriteAllBytes(InstalledBridgePath, bridge);
            File.WriteAllBytes(InstalledHostPath, host);
            File.WriteAllBytes(InstalledManifestPath, manifest);
        }

        public void CreateJunction(string junction, string target)
        {
            var start = new ProcessStartInfo("cmd.exe")
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardError = true,
                RedirectStandardOutput = true
            };
            start.ArgumentList.Add("/c");
            start.ArgumentList.Add("mklink");
            start.ArgumentList.Add("/J");
            start.ArgumentList.Add(junction);
            start.ArgumentList.Add(target);
            using Process process = Process.Start(start)
                ?? throw new IOException("Unable to start junction helper.");
            process.WaitForExit();
            if (process.ExitCode != 0)
            {
                throw new IOException(process.StandardError.ReadToEnd());
            }
            _junctions.Add(junction);
        }

        public void Dispose()
        {
            foreach (var junction in _junctions.OrderByDescending(path => path.Length))
            {
                if (Directory.Exists(junction))
                {
                    Directory.Delete(junction);
                }
            }
            if (Directory.Exists(Container))
            {
                Directory.Delete(Container, recursive: true);
            }
        }
    }
}
