using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
using DSRRandomizer.Foundation.Paths;
using DSRRandomizer.Foundation.Saves;

namespace DSRRandomizer.Launcher.Services;

internal sealed class VirtualSaveAliasConflictException(string message, Exception? inner = null)
    : IOException(message, inner);

internal sealed class VirtualSaveLinkBinding
{
    private readonly IFileAccess _files;
    private readonly IFileMutationLease _mutationLease;
    private readonly string _aliasPath;
    private readonly string _dedicatedPath;
    private readonly string _identity;
    private bool _released;

    private VirtualSaveLinkBinding(
        IFileAccess files,
        IFileMutationLease mutationLease,
        string aliasPath,
        string dedicatedPath,
        string identity)
    {
        _files = files;
        _mutationLease = mutationLease;
        _aliasPath = aliasPath;
        _dedicatedPath = dedicatedPath;
        _identity = identity;
    }

    public static void RemoveStaleAlias(
        LocalDataLayout layout,
        WriteBoundary boundary,
        IFileAccess files,
        string aliasPath,
        string dedicatedPath)
    {
        var paths = ResolveAndValidate(layout, boundary, aliasPath, dedicatedPath);
        if (!File.Exists(paths.AliasPath))
        {
            return;
        }
        if (!File.Exists(paths.DedicatedPath))
        {
            throw new VirtualSaveAliasConflictException(
                "A virtual save alias exists without its dedicated .rmm file.");
        }

        using var lease = files.AcquireMutationLease(
            layout.Root,
            [paths.AliasDirectory, paths.DedicatedDirectory]);
        lease.Verify();
        var aliasIdentity = ReadRegularIdentity(paths.AliasPath);
        var dedicatedIdentity = ReadRegularIdentity(paths.DedicatedPath);
        if (!aliasIdentity.Value.Equals(
                dedicatedIdentity.Value,
                StringComparison.Ordinal))
        {
            throw new VirtualSaveAliasConflictException(
                "The existing virtual .sl2 is not the dedicated .rmm hard link.");
        }
        if (!files.DeleteIfIdentityMatches(paths.AliasPath, dedicatedIdentity.Value)
            || File.Exists(paths.AliasPath)
            || !files.IsSingleLinkFile(paths.DedicatedPath))
        {
            throw new VirtualSaveAliasConflictException(
                "The stale virtual save hard link could not be removed safely.");
        }
    }

    public static VirtualSaveLinkBinding Create(
        LocalDataLayout layout,
        WriteBoundary boundary,
        IFileAccess files,
        string aliasPath,
        string dedicatedPath)
    {
        var paths = ResolveAndValidate(layout, boundary, aliasPath, dedicatedPath);
        Directory.CreateDirectory(paths.AliasDirectory);
        var lease = files.AcquireMutationLease(
            layout.Root,
            [paths.AliasDirectory, paths.DedicatedDirectory]);
        try
        {
            lease.Verify();
            if (File.Exists(paths.AliasPath)
                || !files.IsSingleLinkFile(paths.DedicatedPath))
            {
                throw new VirtualSaveAliasConflictException(
                    "The virtual .sl2 path is occupied or the dedicated .rmm is already linked.");
            }

            var dedicatedIdentity = ReadRegularIdentity(paths.DedicatedPath);
            if (!CreateHardLinkW(paths.AliasPath, paths.DedicatedPath, IntPtr.Zero))
            {
                throw new VirtualSaveAliasConflictException(
                    "The virtual save hard link could not be created.",
                    new Win32Exception(Marshal.GetLastWin32Error()));
            }

            var aliasIdentity = ReadRegularIdentity(paths.AliasPath);
            var linkedDedicatedIdentity = ReadRegularIdentity(paths.DedicatedPath);
            if (!aliasIdentity.Value.Equals(
                    dedicatedIdentity.Value,
                    StringComparison.Ordinal)
                || !linkedDedicatedIdentity.Value.Equals(
                    dedicatedIdentity.Value,
                    StringComparison.Ordinal)
                || aliasIdentity.NumberOfLinks < 2)
            {
                _ = files.DeleteIfIdentityMatches(
                    paths.AliasPath,
                    dedicatedIdentity.Value);
                throw new VirtualSaveAliasConflictException(
                    "The virtual .sl2 did not bind to the exact dedicated .rmm file.");
            }

            return new VirtualSaveLinkBinding(
                files,
                lease,
                paths.AliasPath,
                paths.DedicatedPath,
                dedicatedIdentity.Value);
        }
        catch
        {
            lease.Dispose();
            throw;
        }
    }

    public bool TryRelease()
    {
        if (_released)
        {
            return true;
        }

        try
        {
            _mutationLease.Verify();
            var dedicatedIdentity = ReadRegularIdentity(_dedicatedPath);
            if (!dedicatedIdentity.Value.Equals(_identity, StringComparison.Ordinal))
            {
                return false;
            }
            if (!File.Exists(_aliasPath))
            {
                return _files.IsSingleLinkFile(_dedicatedPath);
            }

            var aliasIdentity = ReadRegularIdentity(_aliasPath);
            if (!aliasIdentity.Value.Equals(_identity, StringComparison.Ordinal)
                || !_files.DeleteIfIdentityMatches(_aliasPath, _identity))
            {
                return false;
            }
            return !File.Exists(_aliasPath)
                && _files.IsSingleLinkFile(_dedicatedPath);
        }
        catch
        {
            return false;
        }
        finally
        {
            _released = true;
            _mutationLease.Dispose();
        }
    }

    private static ResolvedPaths ResolveAndValidate(
        LocalDataLayout layout,
        WriteBoundary boundary,
        string aliasPath,
        string dedicatedPath)
    {
        ArgumentNullException.ThrowIfNull(layout);
        ArgumentNullException.ThrowIfNull(boundary);
        ArgumentNullException.ThrowIfNull(aliasPath);
        ArgumentNullException.ThrowIfNull(dedicatedPath);
        var alias = Path.GetFullPath(aliasPath);
        var dedicated = Path.GetFullPath(dedicatedPath);
        boundary.EnsureAllowed(alias);
        boundary.EnsureAllowed(dedicated);
        var aliasDirectory = Path.GetDirectoryName(alias)
            ?? throw new VirtualSaveAliasConflictException(
                "The virtual save directory could not be resolved.");
        var dedicatedDirectory = Path.GetDirectoryName(dedicated)
            ?? throw new VirtualSaveAliasConflictException(
                "The dedicated save directory could not be resolved.");
        if (!Path.GetFileName(alias).Equals(
                "DRAKS0005.sl2",
                StringComparison.OrdinalIgnoreCase)
            || !Path.GetFileName(dedicated).Equals(
                "DRAKS0005.rmm",
                StringComparison.OrdinalIgnoreCase))
        {
            throw new VirtualSaveAliasConflictException(
                "The virtual and dedicated save names are invalid.");
        }
        return new ResolvedPaths(
            alias,
            dedicated,
            aliasDirectory,
            dedicatedDirectory);
    }

    private static FileIdentity ReadRegularIdentity(string path)
    {
        const uint fileReadAttributes = 0x00000080;
        const uint shareRead = 0x00000001;
        const uint shareWrite = 0x00000002;
        const uint shareDelete = 0x00000004;
        const uint openExisting = 3;
        const uint openReparsePoint = 0x00200000;
        using var handle = CreateFileW(
            path,
            fileReadAttributes,
            shareRead | shareWrite | shareDelete,
            IntPtr.Zero,
            openExisting,
            openReparsePoint,
            IntPtr.Zero);
        if (handle.IsInvalid)
        {
            throw new VirtualSaveAliasConflictException(
                $"The save file identity could not be read: {path}",
                new Win32Exception(Marshal.GetLastWin32Error()));
        }
        if (!GetFileInformationByHandle(handle, out var information))
        {
            throw new VirtualSaveAliasConflictException(
                $"The save file identity could not be verified: {path}",
                new Win32Exception(Marshal.GetLastWin32Error()));
        }
        var attributes = (FileAttributes)information.FileAttributes;
        if ((attributes & (FileAttributes.Directory | FileAttributes.ReparsePoint)) != 0)
        {
            throw new VirtualSaveAliasConflictException(
                $"The save path is not a regular file: {path}");
        }
        return new FileIdentity(
            $"{information.VolumeSerialNumber:x8}:{information.FileIndexHigh:x8}{information.FileIndexLow:x8}",
            information.NumberOfLinks);
    }

    private sealed record ResolvedPaths(
        string AliasPath,
        string DedicatedPath,
        string AliasDirectory,
        string DedicatedDirectory);

    private sealed record FileIdentity(string Value, uint NumberOfLinks);

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
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CreateHardLinkW(
        string fileName,
        string existingFileName,
        IntPtr securityAttributes);
}
