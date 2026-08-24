using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace DSRRandomizer.Foundation.Saves;

public sealed class SystemFileAccess : IFileAccess
{
    public bool Exists(string path) => File.Exists(path);

    public IFileMutationLease AcquireMutationLease(
        string rootPath,
        IReadOnlyCollection<string> directoryPaths) =>
        new DirectoryMutationLease(rootPath, directoryPaths);

    public FileAttributes GetAttributes(string path) => File.GetAttributes(path);

    public Stream Open(string path, FileMode mode, FileAccess access, FileShare share) =>
        new FileStream(
            path,
            mode,
            access,
            share,
            bufferSize: 81920,
            FileOptions.Asynchronous | FileOptions.SequentialScan);

    public async Task<FileIdentityAndHash> IdentityAndHashAsync(
        Stream stream,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(stream);
        if (stream is not FileStream fileStream)
        {
            throw new ArgumentException("A file stream is required for identity verification.", nameof(stream));
        }

        if (!stream.CanSeek)
        {
            throw new ArgumentException("A seekable stream is required for hashing.", nameof(stream));
        }

        var information = GetInformation(fileStream.SafeFileHandle);
        stream.Position = 0;
        var hash = await SHA256.HashDataAsync(stream, cancellationToken);
        stream.Position = 0;
        return new FileIdentityAndHash(
            Identity(information),
            Combine(information.FileSizeHigh, information.FileSizeLow),
            DateTime.FromFileTimeUtc(Combine(information.LastWriteTimeHigh, information.LastWriteTimeLow)),
            Convert.ToHexString(hash).ToLowerInvariant());
    }

    public async Task<FileIdentityAndHash> IdentityAndHashAsync(
        string path,
        CancellationToken cancellationToken)
    {
        await using var stream = Open(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        return await IdentityAndHashAsync(stream, cancellationToken);
    }

    public async Task<CreatedFileIdentity> CopyAndFlushAsync(
        Stream source,
        string destinationPath,
        CancellationToken cancellationToken)
    {
        string? createdIdentity = null;
        try
        {
            source.Position = 0;
            await using var destination = new FileStream(
                destinationPath,
                FileMode.CreateNew,
                FileAccess.Write,
                FileShare.None,
                bufferSize: 81920,
                FileOptions.Asynchronous | FileOptions.SequentialScan);
            createdIdentity = Identity(GetInformation(destination.SafeFileHandle));
            await source.CopyToAsync(destination, cancellationToken);
            await destination.FlushAsync(cancellationToken);
            destination.Flush(flushToDisk: true);
            return new CreatedFileIdentity(createdIdentity);
        }
        catch
        {
            if (createdIdentity is not null)
            {
                TryDeleteCreatedFile(destinationPath, createdIdentity);
            }

            throw;
        }
    }

    public async Task<CreatedFileIdentity> WriteAllBytesAndFlushAsync(
        string path,
        ReadOnlyMemory<byte> bytes,
        CancellationToken cancellationToken)
    {
        string? createdIdentity = null;
        try
        {
            await using var stream = new FileStream(
                path,
                FileMode.CreateNew,
                FileAccess.Write,
                FileShare.None,
                bufferSize: 4096,
                FileOptions.Asynchronous | FileOptions.SequentialScan);
            createdIdentity = Identity(GetInformation(stream.SafeFileHandle));
            await stream.WriteAsync(bytes, cancellationToken);
            await stream.FlushAsync(cancellationToken);
            stream.Flush(flushToDisk: true);
            return new CreatedFileIdentity(createdIdentity);
        }
        catch
        {
            if (createdIdentity is not null)
            {
                TryDeleteCreatedFile(path, createdIdentity);
            }

            throw;
        }
    }

    public bool MoveCreateNewIfIdentityMatches(
        string sourcePath,
        string destinationPath,
        string expectedSourceIdentity) =>
        RenameIfIdentityMatches(
            sourcePath,
            destinationPath,
            expectedSourceIdentity,
            replaceExisting: false);

    public bool ReplaceIfSourceIdentityMatches(
        string sourcePath,
        string destinationPath,
        string expectedSourceIdentity) =>
        RenameIfIdentityMatches(
            sourcePath,
            destinationPath,
            expectedSourceIdentity,
            replaceExisting: true);

    public bool DeleteIfIdentityMatches(string path, string expectedIdentity)
    {
        using var handle = OpenMutationHandle(path);
        var information = GetInformation(handle);
        if (!Identity(information).Equals(expectedIdentity, StringComparison.Ordinal))
        {
            return false;
        }

        var disposition = new FileDispositionInformation { DeleteFile = true };
        if (!SetFileInformationByHandle(
                handle,
                FileInformationClass.FileDispositionInfo,
                ref disposition,
                (uint)Marshal.SizeOf<FileDispositionInformation>()))
        {
            throw new IOException(
                $"Unable to delete owned file: {path}",
                new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error()));
        }

        return true;
    }

    private static ByHandleFileInformation GetInformation(SafeFileHandle handle)
    {
        if (!GetFileInformationByHandle(handle, out var information))
        {
            throw new IOException(
                "Unable to read file identity.",
                new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error()));
        }

        return information;
    }

    private static long Combine(uint high, uint low) =>
        unchecked((long)(((ulong)high << 32) | low));

    private static string Identity(ByHandleFileInformation information) =>
        $"{information.VolumeSerialNumber:x8}:{information.FileIndexHigh:x8}{information.FileIndexLow:x8}";

    private static bool RenameIfIdentityMatches(
        string sourcePath,
        string destinationPath,
        string expectedSourceIdentity,
        bool replaceExisting)
    {
        using var handle = OpenMutationHandle(sourcePath);
        var information = GetInformation(handle);
        if (!Identity(information).Equals(expectedSourceIdentity, StringComparison.Ordinal))
        {
            return false;
        }

        var destination = ToNtPath(Path.GetFullPath(destinationPath));
        var nameBytes = Encoding.Unicode.GetBytes(destination);
        var rootOffset = IntPtr.Size == 8 ? 8 : 4;
        var lengthOffset = rootOffset + IntPtr.Size;
        var nameOffset = lengthOffset + sizeof(uint);
        var bufferSize = nameOffset + nameBytes.Length + sizeof(char);
        var buffer = Marshal.AllocHGlobal(bufferSize);
        try
        {
            for (var index = 0; index < bufferSize; index++)
            {
                Marshal.WriteByte(buffer, index, 0);
            }

            Marshal.WriteByte(buffer, 0, replaceExisting ? (byte)1 : (byte)0);
            Marshal.WriteIntPtr(buffer, rootOffset, IntPtr.Zero);
            Marshal.WriteInt32(buffer, lengthOffset, nameBytes.Length);
            Marshal.Copy(nameBytes, 0, IntPtr.Add(buffer, nameOffset), nameBytes.Length);
            if (!SetFileInformationByHandle(
                    handle,
                    FileInformationClass.FileRenameInfo,
                    buffer,
                    (uint)bufferSize))
            {
                var error = Marshal.GetLastWin32Error();
                throw new IOException(
                    $"Unable to publish owned file to: {destinationPath} (Win32 {error})",
                    new System.ComponentModel.Win32Exception(error));
            }

            return true;
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    private void TryDeleteCreatedFile(string path, string createdIdentity)
    {
        try
        {
            DeleteIfIdentityMatches(path, createdIdentity);
        }
        catch (Exception exception) when (
            exception is IOException or UnauthorizedAccessException or FileNotFoundException)
        {
            // Preserve the original write failure; a replacement is not transaction-owned.
        }
    }

    private static SafeFileHandle OpenMutationHandle(string path)
    {
        const uint delete = 0x00010000;
        const uint fileReadAttributes = 0x00000080;
        const uint shareRead = 0x00000001;
        const uint shareWrite = 0x00000002;
        const uint openExisting = 3;
        const uint openReparsePoint = 0x00200000;
        var handle = CreateFileW(
            path,
            delete | fileReadAttributes,
            shareRead | shareWrite,
            IntPtr.Zero,
            openExisting,
            openReparsePoint,
            IntPtr.Zero);
        if (handle.IsInvalid)
        {
            var error = Marshal.GetLastWin32Error();
            handle.Dispose();
            throw new IOException(
                $"Unable to open owned file for mutation: {path}",
                new System.ComponentModel.Win32Exception(error));
        }

        var information = GetInformation(handle);
        if (((FileAttributes)information.FileAttributes & FileAttributes.ReparsePoint) != 0)
        {
            handle.Dispose();
            throw new UnauthorizedAccessException($"Refusing to mutate a reparse point: {path}");
        }

        return handle;
    }

    private static SafeFileHandle OpenDirectoryMutationHandle(string path)
    {
        const uint fileReadAttributes = 0x00000080;
        const uint openExisting = 3;
        const uint backupSemantics = 0x02000000;
        const uint openReparsePoint = 0x00200000;
        var handle = CreateFileW(
            path,
            fileReadAttributes,
            0,
            IntPtr.Zero,
            openExisting,
            backupSemantics | openReparsePoint,
            IntPtr.Zero);
        if (handle.IsInvalid)
        {
            var error = Marshal.GetLastWin32Error();
            handle.Dispose();
            throw new IOException(
                $"Unable to lock external directory for mutation: {path}",
                new System.ComponentModel.Win32Exception(error));
        }

        var information = GetInformation(handle);
        var attributes = (FileAttributes)information.FileAttributes;
        if ((attributes & FileAttributes.Directory) == 0
            || (attributes & FileAttributes.ReparsePoint) != 0)
        {
            handle.Dispose();
            throw new UnauthorizedAccessException(
                $"External mutation directory is not a regular directory: {path}");
        }

        return handle;
    }

    private static string ToNtPath(string fullPath) =>
        fullPath.StartsWith(@"\\", StringComparison.Ordinal)
            ? @"\??\UNC\" + fullPath[2..]
            : @"\??\" + fullPath;

    private sealed class DirectoryMutationLease : IFileMutationLease
    {
        private readonly List<(string Path, SafeFileHandle Handle)> _handles = [];

        public DirectoryMutationLease(
            string rootPath,
            IReadOnlyCollection<string> directoryPaths)
        {
            ArgumentException.ThrowIfNullOrWhiteSpace(rootPath);
            ArgumentNullException.ThrowIfNull(directoryPaths);

            var root = Path.GetFullPath(rootPath).TrimEnd(Path.DirectorySeparatorChar);
            if (!Directory.Exists(root))
            {
                throw new DirectoryNotFoundException($"External mutation root is missing: {root}");
            }

            var directories = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { root };
            foreach (var directoryPath in directoryPaths)
            {
                var target = Path.GetFullPath(directoryPath).TrimEnd(Path.DirectorySeparatorChar);
                if (!IsAtOrBelow(target, root))
                {
                    throw new UnauthorizedAccessException(
                        $"Mutation directory is outside the external root: {target}");
                }

                var relative = Path.GetRelativePath(root, target);
                var current = root;
                if (!relative.Equals(".", StringComparison.Ordinal))
                {
                    foreach (var segment in relative.Split(
                                 Path.DirectorySeparatorChar,
                                 StringSplitOptions.RemoveEmptyEntries))
                    {
                        current = Path.Combine(current, segment);
                        directories.Add(current);
                    }
                }
            }

            try
            {
                foreach (var directory in directories.OrderBy(PathDepth))
                {
                    if (!Directory.Exists(directory))
                    {
                        Directory.CreateDirectory(directory);
                    }

                    _handles.Add((
                        Path.GetFullPath(directory).TrimEnd(Path.DirectorySeparatorChar),
                        OpenDirectoryMutationHandle(directory)));
                }
            }
            catch
            {
                Dispose();
                throw;
            }
        }

        public void Verify()
        {
            foreach (var (expectedPath, handle) in _handles)
            {
                var actualPath = ResolveFinalPath(handle);
                if (!actualPath.Equals(expectedPath, StringComparison.OrdinalIgnoreCase))
                {
                    throw new UnauthorizedAccessException(
                        $"External mutation directory identity changed: {expectedPath}");
                }
            }
        }

        public void Dispose()
        {
            for (var index = _handles.Count - 1; index >= 0; index--)
            {
                _handles[index].Handle.Dispose();
            }

            _handles.Clear();
        }

        private static bool IsAtOrBelow(string candidate, string root) =>
            candidate.Equals(root, StringComparison.OrdinalIgnoreCase)
            || candidate.StartsWith(
                root + Path.DirectorySeparatorChar,
                StringComparison.OrdinalIgnoreCase);

        private static int PathDepth(string path) =>
            path.Count(character => character == Path.DirectorySeparatorChar);

        private static string ResolveFinalPath(SafeFileHandle handle)
        {
            var capacity = 512;
            while (true)
            {
                var buffer = new StringBuilder(capacity);
                var length = GetFinalPathNameByHandleW(
                    handle,
                    buffer,
                    (uint)buffer.Capacity,
                    0);
                if (length == 0)
                {
                    throw new IOException(
                        "Unable to verify external mutation directory identity.",
                        new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error()));
                }

                if (length < buffer.Capacity)
                {
                    const string devicePrefix = @"\\?\";
                    var value = buffer.ToString();
                    return (value.StartsWith(devicePrefix, StringComparison.OrdinalIgnoreCase)
                            ? value[devicePrefix.Length..]
                            : value)
                        .TrimEnd(Path.DirectorySeparatorChar);
                }

                capacity = checked((int)length + 1);
            }
        }
    }

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

    [StructLayout(LayoutKind.Sequential)]
    private struct FileDispositionInformation
    {
        [MarshalAs(UnmanagedType.Bool)]
        public bool DeleteFile;
    }

    private enum FileInformationClass
    {
        FileRenameInfo = 3,
        FileDispositionInfo = 4
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetFileInformationByHandle(
        SafeFileHandle file,
        out ByHandleFileInformation fileInformation);

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
    private static extern bool SetFileInformationByHandle(
        SafeFileHandle file,
        FileInformationClass fileInformationClass,
        IntPtr fileInformation,
        uint bufferSize);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetFileInformationByHandle(
        SafeFileHandle file,
        FileInformationClass fileInformationClass,
        ref FileDispositionInformation fileInformation,
        uint bufferSize);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern uint GetFinalPathNameByHandleW(
        SafeFileHandle file,
        StringBuilder filePath,
        uint filePathLength,
        uint flags);
}
