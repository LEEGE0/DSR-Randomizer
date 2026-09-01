using System.Runtime.InteropServices;
using System.Security;
using System.Text;
using System.Text.Json;
using DSRRandomizer.Launcher.Safety;
using Microsoft.Win32.SafeHandles;

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
    private readonly Action? _onDestinationLocked;
    private readonly Action? _beforeFinalVerification;

    public RmmBridgeBundleInstaller(string packageRoot, LaunchArtifactIdentities identities)
        : this(packageRoot, identities, null, null)
    {
    }

    internal RmmBridgeBundleInstaller(
        string packageRoot,
        LaunchArtifactIdentities identities,
        Action? onDestinationLocked,
        Action? beforeFinalVerification)
    {
        _packageRoot = packageRoot;
        _identities = identities ?? throw new ArgumentNullException(nameof(identities));
        _onDestinationLocked = onDestinationLocked;
        _beforeFinalVerification = beforeFinalVerification;
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
            using var destinationLease = DestinationDirectoryLease.Acquire(
                destination.Root,
                destination.BundleRoot);
            _onDestinationLocked?.Invoke();
            if (InstalledMatches(destination, bundle))
            {
                return RmmBridgeInstallResult.Ready(changed: false);
            }
            RejectDestinationReparsePoints(destination);
            Install(destination, bundle);

            try
            {
                _beforeFinalVerification?.Invoke();
                return InstalledMatches(destination, bundle)
                    ? RmmBridgeInstallResult.Ready(changed: true)
                    : RmmBridgeInstallResult.Failed(InstallTampered);
            }
            catch (Exception exception) when (IsExpectedFailure(exception))
            {
                return RmmBridgeInstallResult.Failed(InstallTampered);
            }
        }
        catch (Exception exception) when (IsExpectedFailure(exception))
        {
            return RmmBridgeInstallResult.Failed(InstallFailed);
        }
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

            RejectDestinationReparsePoints(destination);
            File.Move(bridgeTemporary, destination.BridgePath, overwrite: true);
            RejectDestinationReparsePoints(destination);
            File.Move(hostTemporary, destination.HostPath, overwrite: true);
            RejectDestinationReparsePoints(destination);
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

    private static void RejectDestinationReparsePoints(Destination destination)
    {
        RejectReparsePoint(destination.BridgePath);
        RejectReparsePoint(destination.HostPath);
        RejectReparsePoint(destination.ManifestPath);
    }

    private static void RejectReparsePoint(string path)
    {
        if ((File.Exists(path) || Directory.Exists(path))
            && (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
        {
            throw new UnauthorizedAccessException($"The bridge destination is redirected: {path}");
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

    private sealed class DestinationDirectoryLease : IDisposable
    {
        private readonly List<SafeFileHandle> _handles;

        private DestinationDirectoryLease(List<SafeFileHandle> handles)
        {
            _handles = handles;
        }

        public static DestinationDirectoryLease Acquire(string externalRoot, string bundleRoot)
        {
            if (!Directory.Exists(externalRoot))
            {
                throw new DirectoryNotFoundException(
                    $"The selected external root does not exist: {externalRoot}");
            }
            EnsureContained(externalRoot, bundleRoot);

            var root = Path.GetPathRoot(externalRoot)
                ?? throw new IOException($"The external root is invalid: {externalRoot}");
            var handles = new List<SafeFileHandle>();
            try
            {
                var current = root;
                handles.Add(OpenVerifiedDirectory(current));
                foreach (var segment in Path.GetRelativePath(root, externalRoot).Split(
                             Path.DirectorySeparatorChar,
                             StringSplitOptions.RemoveEmptyEntries))
                {
                    current = Path.Combine(current, segment);
                    handles.Add(OpenVerifiedDirectory(current));
                }

                foreach (var segment in Path.GetRelativePath(externalRoot, bundleRoot).Split(
                             Path.DirectorySeparatorChar,
                             StringSplitOptions.RemoveEmptyEntries))
                {
                    current = Path.Combine(current, segment);
                    if (!Directory.Exists(current))
                    {
                        Directory.CreateDirectory(current);
                    }
                    handles.Add(OpenVerifiedDirectory(current));
                }
                return new DestinationDirectoryLease(handles);
            }
            catch
            {
                DisposeHandles(handles);
                throw;
            }
        }

        public void Dispose() => DisposeHandles(_handles);

        private static SafeFileHandle OpenVerifiedDirectory(string path)
        {
            var handle = CreateFileW(
                path,
                FileReadAttributes,
                ShareRead | ShareWrite,
                IntPtr.Zero,
                OpenExisting,
                BackupSemantics | OpenReparsePoint,
                IntPtr.Zero);
            if (handle.IsInvalid)
            {
                var error = Marshal.GetLastWin32Error();
                handle.Dispose();
                throw new IOException(
                    $"Unable to lock bridge destination directory: {path}",
                    new System.ComponentModel.Win32Exception(error));
            }

            try
            {
                if (!GetFileInformationByHandle(handle, out var information))
                {
                    throw new IOException(
                        $"Unable to inspect bridge destination directory: {path}",
                        new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error()));
                }
                var attributes = (FileAttributes)information.FileAttributes;
                if ((attributes & FileAttributes.Directory) == 0
                    || (attributes & FileAttributes.ReparsePoint) != 0
                    || !ResolveFinalPath(handle).Equals(
                        Path.GetFullPath(path).TrimEnd(Path.DirectorySeparatorChar),
                        StringComparison.OrdinalIgnoreCase))
                {
                    throw new UnauthorizedAccessException(
                        $"The bridge destination directory is redirected: {path}");
                }
                return handle;
            }
            catch
            {
                handle.Dispose();
                throw;
            }
        }

        private static string ResolveFinalPath(SafeFileHandle handle)
        {
            var capacity = 512;
            while (true)
            {
                var buffer = new StringBuilder(capacity);
                var length = GetFinalPathNameByHandleW(handle, buffer, (uint)capacity, 0);
                if (length == 0)
                {
                    throw new IOException(
                        "Unable to resolve bridge destination identity.",
                        new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error()));
                }
                if (length < capacity)
                {
                    var value = buffer.ToString();
                    const string uncPrefix = @"\\?\UNC\";
                    const string devicePrefix = @"\\?\";
                    if (value.StartsWith(uncPrefix, StringComparison.OrdinalIgnoreCase))
                    {
                        value = @"\\" + value[uncPrefix.Length..];
                    }
                    else if (value.StartsWith(devicePrefix, StringComparison.OrdinalIgnoreCase))
                    {
                        value = value[devicePrefix.Length..];
                    }
                    return value.TrimEnd(Path.DirectorySeparatorChar);
                }
                capacity = checked((int)length + 1);
            }
        }

        private static void DisposeHandles(List<SafeFileHandle> handles)
        {
            for (var index = handles.Count - 1; index >= 0; index--)
            {
                handles[index].Dispose();
            }
            handles.Clear();
        }

        private const uint FileReadAttributes = 0x00000080;
        private const uint ShareRead = 0x00000001;
        private const uint ShareWrite = 0x00000002;
        private const uint OpenExisting = 3;
        private const uint BackupSemantics = 0x02000000;
        private const uint OpenReparsePoint = 0x00200000;

        [StructLayout(LayoutKind.Sequential)]
        private struct ByHandleFileInformation
        {
            public uint FileAttributes;
            public uint CreationTimeLow;
            public uint CreationTimeHigh;
            public uint LastAccessTimeLow;
            public uint LastAccessTimeHigh;
            public uint LastWriteTimeLow;
            public uint LastWriteTimeHigh;
            public uint VolumeSerialNumber;
            public uint FileSizeHigh;
            public uint FileSizeLow;
            public uint NumberOfLinks;
            public uint FileIndexHigh;
            public uint FileIndexLow;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern SafeFileHandle CreateFileW(
            string fileName,
            uint desiredAccess,
            uint shareMode,
            IntPtr securityAttributes,
            uint creationDisposition,
            uint flagsAndAttributes,
            IntPtr templateFile);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GetFileInformationByHandle(
            SafeFileHandle file,
            out ByHandleFileInformation fileInformation);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern uint GetFinalPathNameByHandleW(
            SafeFileHandle file,
            StringBuilder filePath,
            uint filePathLength,
            uint flags);
    }
}
