using System.Security;
using System.Text.Json;
using DSRRandomizer.Launcher.Safety;

namespace DSRRandomizer.Launcher.Services;

internal sealed class RmmBridgeBundleInstaller : IRmmBridgeBundleInstaller
{
    private const string BridgeRelativePath = "components/rmm-bridge/DSRRandomizer.RmmBridge.dll";
    private const string HostRelativePath = "components/rmm-bridge/DSRRandomizer.RmmBridgeHost.exe";
    private const string ManifestRelativePath = "components/rmm-bridge/deployment-manifest.json";
    private const string BundleInvalid = "RMM_BRIDGE_BUNDLE_INVALID";
    private const string InstallFailed = "RMM_BRIDGE_INSTALL_FAILED";
    private const string InstallTampered = "RMM_BRIDGE_INSTALL_TAMPERED";

    private readonly string _packageRoot;
    private readonly LaunchArtifactIdentities _identities;

    public RmmBridgeBundleInstaller(string packageRoot, LaunchArtifactIdentities identities)
    {
        _packageRoot = packageRoot;
        _identities = identities ?? throw new ArgumentNullException(nameof(identities));
    }

    public RmmBridgeInstallResult EnsureInstalled(string externalRoot)
    {
        Bundle bundle;
        try
        {
            if (!TryOpenBundle(out bundle))
            {
                return RmmBridgeInstallResult.Failed(BundleInvalid);
            }
        }
        catch (Exception exception) when (IsExpectedFailure(exception))
        {
            return RmmBridgeInstallResult.Failed(BundleInvalid);
        }

        Destination destination;
        try
        {
            destination = ResolveDestination(externalRoot);
            EnsureSafeDestination(destination, requireBundleDirectory: false);
            if (InstalledMatches(destination, bundle))
            {
                return RmmBridgeInstallResult.Ready(changed: false);
            }
            PrepareDestination(destination);
            Install(destination, bundle);
        }
        catch (Exception exception) when (IsExpectedFailure(exception))
        {
            return RmmBridgeInstallResult.Failed(InstallFailed);
        }

        return InstalledMatches(destination, bundle)
            ? RmmBridgeInstallResult.Ready(changed: true)
            : RmmBridgeInstallResult.Failed(InstallTampered);
    }

    private bool TryOpenBundle(out Bundle bundle)
    {
        bundle = default;
        var packageRoot = CanonicalizeRoot(_packageRoot, rejectParentSegments: false);
        var bridgePath = ResolveContained(packageRoot, BridgeRelativePath);
        var hostPath = ResolveContained(packageRoot, HostRelativePath);
        var manifestPath = ResolveContained(packageRoot, ManifestRelativePath);

        using var bridge = LaunchArtifactLease.TryOpen(bridgePath);
        using var host = LaunchArtifactLease.TryOpen(hostPath);
        using var manifest = LaunchArtifactLease.TryOpen(manifestPath);
        if (bridge is null
            || host is null
            || manifest is null
            || !bridge.Sha256.Equals(_identities.BridgeSha256, StringComparison.Ordinal)
            || !host.Sha256.Equals(_identities.HostSha256, StringComparison.Ordinal)
            || !ManifestMatches(
                manifest.Bytes,
                _identities.BridgeSha256,
                _identities.HostSha256))
        {
            return false;
        }

        bundle = new Bundle(
            bridge.Bytes,
            host.Bytes,
            manifest.Bytes,
            bridge.Sha256,
            host.Sha256,
            manifest.Sha256);
        return true;
    }

    private static Destination ResolveDestination(string externalRoot)
    {
        var root = CanonicalizeRoot(externalRoot, rejectParentSegments: true);
        return new Destination(
            root,
            Path.GetDirectoryName(ResolveContained(root, BridgeRelativePath))
                ?? throw new IOException("The bridge destination directory is invalid."),
            ResolveContained(root, BridgeRelativePath),
            ResolveContained(root, HostRelativePath),
            ResolveContained(root, ManifestRelativePath));
    }

    private static void PrepareDestination(Destination destination)
    {
        if (!Directory.Exists(destination.Root))
        {
            throw new DirectoryNotFoundException(
                $"The selected external root does not exist: {destination.Root}");
        }

        var components = Path.GetDirectoryName(destination.BundleRoot)
            ?? throw new IOException("The bridge components directory is invalid.");
        Directory.CreateDirectory(components);
        EnsureNoReparseAncestors(components);
        Directory.CreateDirectory(destination.BundleRoot);
        EnsureSafeDestination(destination, requireBundleDirectory: true);
    }

    private static void Install(Destination destination, Bundle bundle)
    {
        var bridgeTemporary = TemporarySibling(destination.BridgePath);
        var hostTemporary = TemporarySibling(destination.HostPath);
        var manifestTemporary = TemporarySibling(destination.ManifestPath);
        try
        {
            WriteTemporary(bridgeTemporary, bundle.BridgeBytes, bundle.BridgeSha256);
            WriteTemporary(hostTemporary, bundle.HostBytes, bundle.HostSha256);
            WriteTemporary(manifestTemporary, bundle.ManifestBytes, bundle.ManifestSha256);

            EnsureSafeDestination(destination, requireBundleDirectory: true);
            File.Move(bridgeTemporary, destination.BridgePath, overwrite: true);
            EnsureSafeDestination(destination, requireBundleDirectory: true);
            File.Move(hostTemporary, destination.HostPath, overwrite: true);
            EnsureSafeDestination(destination, requireBundleDirectory: true);
            File.Move(manifestTemporary, destination.ManifestPath, overwrite: true);
        }
        finally
        {
            TryDelete(bridgeTemporary);
            TryDelete(hostTemporary);
            TryDelete(manifestTemporary);
        }
    }

    private static void WriteTemporary(string path, byte[] bytes, string expectedSha256)
    {
        EnsureNoReparseAncestors(path);
        using (var stream = new FileStream(
                   path,
                   new FileStreamOptions
                   {
                       Mode = FileMode.CreateNew,
                       Access = FileAccess.Write,
                       Share = FileShare.None,
                       BufferSize = 81920,
                       Options = FileOptions.WriteThrough
                   }))
        {
            stream.Write(bytes);
            stream.Flush(flushToDisk: true);
        }

        using var lease = LaunchArtifactLease.TryOpen(path);
        if (lease is null || !lease.Sha256.Equals(expectedSha256, StringComparison.Ordinal))
        {
            throw new IOException("A staged bridge artifact failed verification.");
        }
    }

    private static bool InstalledMatches(Destination destination, Bundle bundle)
    {
        using var bridge = LaunchArtifactLease.TryOpen(destination.BridgePath);
        using var host = LaunchArtifactLease.TryOpen(destination.HostPath);
        using var manifest = LaunchArtifactLease.TryOpen(destination.ManifestPath);
        return bridge is not null
               && host is not null
               && manifest is not null
               && bridge.Sha256.Equals(bundle.BridgeSha256, StringComparison.Ordinal)
               && host.Sha256.Equals(bundle.HostSha256, StringComparison.Ordinal)
               && manifest.Sha256.Equals(bundle.ManifestSha256, StringComparison.Ordinal)
               && ManifestMatches(
                   manifest.Bytes,
                   bundle.BridgeSha256,
                   bundle.HostSha256);
    }

    private static bool ManifestMatches(
        byte[] bytes,
        string expectedBridgeSha256,
        string expectedHostSha256)
    {
        try
        {
            using var document = JsonDocument.Parse(bytes);
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
            {
                return false;
            }

            var properties = root.EnumerateObject().ToArray();
            if (properties.Length != 4
                || properties.Any(property => property.Name is not (
                    "schemaVersion" or "configuration" or "bridgeSha256" or "hostSha256")))
            {
                return false;
            }

            return root.TryGetProperty("schemaVersion", out var schemaVersion)
                   && schemaVersion.ValueKind == JsonValueKind.Number
                   && schemaVersion.TryGetInt32(out var schema)
                   && schema == 1
                   && root.TryGetProperty("configuration", out var configuration)
                   && configuration.ValueKind == JsonValueKind.String
                   && configuration.GetString() == "Release"
                   && root.TryGetProperty("bridgeSha256", out var bridgeHash)
                   && bridgeHash.ValueKind == JsonValueKind.String
                   && bridgeHash.GetString() == expectedBridgeSha256
                   && root.TryGetProperty("hostSha256", out var hostHash)
                   && hostHash.ValueKind == JsonValueKind.String
                   && hostHash.GetString() == expectedHostSha256;
        }
        catch (JsonException)
        {
            return false;
        }
    }

    private static void EnsureSafeDestination(
        Destination destination,
        bool requireBundleDirectory)
    {
        EnsureContained(destination.Root, destination.BundleRoot);
        EnsureContained(destination.Root, destination.BridgePath);
        EnsureContained(destination.Root, destination.HostPath);
        EnsureContained(destination.Root, destination.ManifestPath);
        EnsureNoReparseAncestors(destination.Root);
        EnsureNoReparseAncestors(destination.BundleRoot);
        EnsureNoReparseAncestors(destination.BridgePath);
        EnsureNoReparseAncestors(destination.HostPath);
        EnsureNoReparseAncestors(destination.ManifestPath);
        if (requireBundleDirectory && !Directory.Exists(destination.BundleRoot))
        {
            throw new DirectoryNotFoundException(
                $"The bridge destination directory does not exist: {destination.BundleRoot}");
        }
    }

    private static string CanonicalizeRoot(string root, bool rejectParentSegments)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(root);
        if (!Path.IsPathFullyQualified(root))
        {
            throw new ArgumentException("The root path must be fully qualified.", nameof(root));
        }
        if (rejectParentSegments && HasParentSegment(root))
        {
            throw new UnauthorizedAccessException("The external root path contains a parent escape.");
        }
        return Path.TrimEndingDirectorySeparator(Path.GetFullPath(root));
    }

    private static bool HasParentSegment(string path)
    {
        var root = Path.GetPathRoot(path) ?? string.Empty;
        return path[root.Length..]
            .Split(
                [Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar],
                StringSplitOptions.RemoveEmptyEntries)
            .Any(segment => segment.Equals("..", StringComparison.Ordinal));
    }

    private static string ResolveContained(string root, string relativePath)
    {
        var path = Path.GetFullPath(Path.Combine(
            root,
            relativePath.Replace('/', Path.DirectorySeparatorChar)));
        EnsureContained(root, path);
        return path;
    }

    private static void EnsureContained(string root, string path)
    {
        var relative = Path.GetRelativePath(root, path);
        if (Path.IsPathRooted(relative)
            || relative.Equals("..", StringComparison.Ordinal)
            || relative.StartsWith($"..{Path.DirectorySeparatorChar}", StringComparison.Ordinal))
        {
            throw new UnauthorizedAccessException("The bridge path escapes the selected root.");
        }
    }

    private static void EnsureNoReparseAncestors(string path)
    {
        var fullPath = Path.GetFullPath(path);
        var root = Path.GetPathRoot(fullPath)
            ?? throw new IOException($"The path root is invalid: {fullPath}");
        var current = root;
        var relative = Path.GetRelativePath(root, fullPath);
        foreach (var segment in relative.Split(
                     Path.DirectorySeparatorChar,
                     StringSplitOptions.RemoveEmptyEntries))
        {
            current = Path.Combine(current, segment);
            if (!File.Exists(current) && !Directory.Exists(current))
            {
                continue;
            }
            if ((File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0)
            {
                throw new UnauthorizedAccessException($"The bridge path is redirected: {current}");
            }
        }
    }

    private static string TemporarySibling(string destinationPath) =>
        $"{destinationPath}.{Guid.NewGuid():N}.tmp";

    private static void TryDelete(string path)
    {
        try
        {
            File.Delete(path);
        }
        catch (Exception exception) when (IsExpectedFailure(exception))
        {
        }
    }

    private static bool IsExpectedFailure(Exception exception) => exception is
        IOException
        or UnauthorizedAccessException
        or ArgumentException
        or NotSupportedException
        or SecurityException
        or JsonException;

    private readonly record struct Bundle(
        byte[] BridgeBytes,
        byte[] HostBytes,
        byte[] ManifestBytes,
        string BridgeSha256,
        string HostSha256,
        string ManifestSha256);

    private readonly record struct Destination(
        string Root,
        string BundleRoot,
        string BridgePath,
        string HostPath,
        string ManifestPath);
}
