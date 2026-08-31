Set-StrictMode -Version Latest

Import-Module (Join-Path $PSScriptRoot 'SafeReleaseDirectories.psm1')
Add-Type -AssemblyName System.IO.Compression

if ($null -eq ('DSRRandomizer.Packaging.RedistributableArtifactLease' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace DSRRandomizer.Packaging
{
    public sealed class RedistributableArtifactLease : IDisposable
    {
        private FileStream stream;
        private bool disposed;

        private RedistributableArtifactLease(string path, FileStream stream)
        {
            Path = path;
            this.stream = stream;
        }

        public string Path { get; }
        public Stream Stream
        {
            get
            {
                ThrowIfDisposed();
                stream.Position = 0;
                return stream;
            }
        }

        public static RedistributableArtifactLease Acquire(
            string path,
            string description)
        {
            var fullPath = System.IO.Path.GetFullPath(path);
            var handle = CreateFileW(
                fullPath,
                GenericRead,
                ShareRead,
                IntPtr.Zero,
                OpenExisting,
                OpenReparsePoint | SequentialScan,
                IntPtr.Zero);
            if (handle.IsInvalid)
            {
                var error = Marshal.GetLastWin32Error();
                handle.Dispose();
                throw new IOException(
                    $"Unable to lease {description}: {fullPath}",
                    new Win32Exception(error));
            }

            FileStream leasedStream = null;
            try
            {
                if (!GetFileInformationByHandle(handle, out var information))
                {
                    throw new IOException(
                        $"Unable to inspect {description}: {fullPath}",
                        new Win32Exception(Marshal.GetLastWin32Error()));
                }
                var attributes = (FileAttributes)information.FileAttributes;
                if ((attributes & FileAttributes.Directory) != 0
                    || (attributes & FileAttributes.ReparsePoint) != 0)
                {
                    throw new UnauthorizedAccessException(
                        $"The {description} is a reparse point or not a regular file: {fullPath}");
                }
                if (information.NumberOfLinks != 1)
                {
                    throw new UnauthorizedAccessException(
                        $"The {description} has multiple hard links: {fullPath}");
                }
                var finalPath = ResolveFinalPath(handle);
                if (!finalPath.Equals(Normalize(fullPath), StringComparison.OrdinalIgnoreCase))
                {
                    throw new UnauthorizedAccessException(
                        $"The {description} resolves outside its lexical path: {fullPath}");
                }

                leasedStream = new FileStream(handle, FileAccess.Read, 1024 * 1024, false);
                return new RedistributableArtifactLease(fullPath, leasedStream);
            }
            catch
            {
                if (leasedStream != null)
                {
                    leasedStream.Dispose();
                }
                else
                {
                    handle.Dispose();
                }
                throw;
            }
        }

        public string ComputeSha256()
        {
            ThrowIfDisposed();
            stream.Position = 0;
            var result = Convert.ToHexString(SHA256.HashData(stream)).ToLowerInvariant();
            stream.Position = 0;
            return result;
        }

        public void CopyTo(Stream destination)
        {
            ThrowIfDisposed();
            stream.Position = 0;
            stream.CopyTo(destination, 1024 * 1024);
            stream.Position = 0;
        }

        public void Dispose()
        {
            if (disposed)
            {
                return;
            }
            disposed = true;
            stream.Dispose();
            stream = null;
        }

        private void ThrowIfDisposed()
        {
            if (disposed)
            {
                throw new ObjectDisposedException(nameof(RedistributableArtifactLease));
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
                        "Unable to resolve release artifact identity.",
                        new Win32Exception(Marshal.GetLastWin32Error()));
                }
                if (length < capacity)
                {
                    return Normalize(buffer.ToString());
                }
                capacity = checked((int)length + 1);
            }
        }

        private static string Normalize(string path)
        {
            const string uncPrefix = @"\\?\UNC\";
            const string devicePrefix = @"\\?\";
            var value = path;
            if (value.StartsWith(uncPrefix, StringComparison.OrdinalIgnoreCase))
            {
                value = @"\\" + value.Substring(uncPrefix.Length);
            }
            else if (value.StartsWith(devicePrefix, StringComparison.OrdinalIgnoreCase))
            {
                value = value.Substring(devicePrefix.Length);
            }
            return System.IO.Path.GetFullPath(value);
        }

        private const uint GenericRead = 0x80000000;
        private const uint ShareRead = 0x00000001;
        private const uint OpenExisting = 3;
        private const uint SequentialScan = 0x08000000;
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

    public sealed class RedistributableOwnedFile : IDisposable
    {
        private FileStream stream;
        private bool disposed;

        private RedistributableOwnedFile(
            string path,
            string description,
            FileStream stream)
        {
            Path = path;
            Description = description;
            this.stream = stream;
        }

        public string Path { get; private set; }
        public string Description { get; }
        public Stream Stream
        {
            get
            {
                ThrowIfDisposed();
                stream.Position = 0;
                return stream;
            }
        }

        public static RedistributableOwnedFile CreateFrom(
            RedistributableArtifactLease source,
            string path,
            string description)
        {
            if (source == null)
            {
                throw new ArgumentNullException(nameof(source));
            }
            return Create(path, description, source.CopyTo);
        }

        public static RedistributableOwnedFile CreateFrom(
            RedistributableOwnedFile source,
            string path,
            string description)
        {
            if (source == null)
            {
                throw new ArgumentNullException(nameof(source));
            }
            return Create(path, description, source.CopyTo);
        }

        public static RedistributableOwnedFile OpenExisting(
            string path,
            string description)
        {
            return Open(path, description, CreateDisposition.OpenExisting, null);
        }

        private static RedistributableOwnedFile Create(
            string path,
            string description,
            Action<Stream> copy)
        {
            return Open(path, description, CreateDisposition.CreateNew, copy);
        }

        private static RedistributableOwnedFile Open(
            string path,
            string description,
            CreateDisposition disposition,
            Action<Stream> copy)
        {
            var fullPath = System.IO.Path.GetFullPath(path);
            var creating = disposition == CreateDisposition.CreateNew;
            var handle = CreateFileW(
                ToExtendedPath(fullPath),
                GenericRead | DeleteAccess | (creating ? GenericWrite : 0),
                ShareRead,
                IntPtr.Zero,
                (uint)disposition,
                OpenReparsePoint | SequentialScan | WriteThrough,
                IntPtr.Zero);
            if (handle.IsInvalid)
            {
                var error = Marshal.GetLastWin32Error();
                handle.Dispose();
                throw new IOException(
                    $"Unable to open owned {description}: {fullPath}",
                    new Win32Exception(error));
            }

            FileStream ownedStream = null;
            try
            {
                AssertSafeRegularFile(handle, fullPath, description);
                ownedStream = new FileStream(
                    handle,
                    creating ? FileAccess.ReadWrite : FileAccess.Read,
                    1024 * 1024,
                    false);
                if (copy != null)
                {
                    copy(ownedStream);
                    ownedStream.Flush(true);
                    ownedStream.Position = 0;
                }
                return new RedistributableOwnedFile(
                    fullPath,
                    description,
                    ownedStream);
            }
            catch
            {
                try
                {
                    if (creating)
                    {
                        SetDeleteDisposition(handle);
                    }
                }
                catch
                {
                    // Preserve the original creation/copy failure.
                }
                if (ownedStream != null)
                {
                    ownedStream.Dispose();
                }
                else
                {
                    handle.Dispose();
                }
                throw;
            }
        }

        public string ComputeSha256()
        {
            ThrowIfDisposed();
            stream.Position = 0;
            var result = Convert.ToHexString(SHA256.HashData(stream)).ToLowerInvariant();
            stream.Position = 0;
            return result;
        }

        public void CopyTo(Stream destination)
        {
            ThrowIfDisposed();
            stream.Position = 0;
            stream.CopyTo(destination, 1024 * 1024);
            stream.Position = 0;
        }

        public void RenameTo(string destinationPath, bool replaceExisting)
        {
            ThrowIfDisposed();
            var destination = System.IO.Path.GetFullPath(destinationPath);
            if (!System.IO.Path.GetDirectoryName(Path).Equals(
                    System.IO.Path.GetDirectoryName(destination),
                    StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidOperationException(
                    "Owned release rename must remain inside one output directory.");
            }
            if (stream.CanWrite)
            {
                stream.Flush(true);
            }
            RenameByHandle(stream.SafeFileHandle, destination, replaceExisting);
            var resolved = ResolveFinalPath(stream.SafeFileHandle);
            if (!resolved.Equals(destination, StringComparison.OrdinalIgnoreCase))
            {
                throw new IOException(
                    $"Owned release rename resolved to an unexpected path: {resolved}");
            }
            Path = destination;
            stream.Position = 0;
        }

        public void DeleteOnClose()
        {
            ThrowIfDisposed();
            SetDeleteDisposition(stream.SafeFileHandle);
        }

        public void Dispose()
        {
            if (disposed)
            {
                return;
            }
            disposed = true;
            stream.Dispose();
            stream = null;
        }

        private void ThrowIfDisposed()
        {
            if (disposed)
            {
                throw new ObjectDisposedException(nameof(RedistributableOwnedFile));
            }
        }

        private static void AssertSafeRegularFile(
            SafeFileHandle handle,
            string fullPath,
            string description)
        {
            if (!GetFileInformationByHandle(handle, out var information))
            {
                throw new IOException(
                    $"Unable to inspect owned {description}: {fullPath}",
                    new Win32Exception(Marshal.GetLastWin32Error()));
            }
            var attributes = (FileAttributes)information.FileAttributes;
            if ((attributes & FileAttributes.Directory) != 0
                || (attributes & FileAttributes.ReparsePoint) != 0)
            {
                throw new UnauthorizedAccessException(
                    $"The owned {description} is a reparse point or not a regular file: {fullPath}");
            }
            if (information.NumberOfLinks != 1)
            {
                throw new UnauthorizedAccessException(
                    $"The owned {description} has multiple hard links: {fullPath}");
            }
            if (!ResolveFinalPath(handle).Equals(
                    fullPath,
                    StringComparison.OrdinalIgnoreCase))
            {
                throw new UnauthorizedAccessException(
                    $"The owned {description} resolves outside its lexical path: {fullPath}");
            }
        }

        private static void RenameByHandle(
            SafeFileHandle handle,
            string destinationPath,
            bool replaceExisting)
        {
            var destination = ToExtendedPath(destinationPath);
            var nameBytes = Encoding.Unicode.GetBytes(destination);
            var rootOffset = IntPtr.Size == 8 ? 8 : 4;
            var lengthOffset = IntPtr.Size == 8 ? 16 : 8;
            var nameOffset = IntPtr.Size == 8 ? 20 : 12;
            var bufferLength = checked(nameOffset + nameBytes.Length + sizeof(char));
            var buffer = Marshal.AllocHGlobal(bufferLength);
            try
            {
                for (var index = 0; index < bufferLength; index++)
                {
                    Marshal.WriteByte(buffer, index, 0);
                }
                Marshal.WriteInt32(buffer, 0, replaceExisting ? 1 : 0);
                Marshal.WriteIntPtr(buffer, rootOffset, IntPtr.Zero);
                Marshal.WriteInt32(buffer, lengthOffset, nameBytes.Length);
                Marshal.Copy(nameBytes, 0, IntPtr.Add(buffer, nameOffset), nameBytes.Length);
                if (!SetFileInformationByHandle(
                        handle,
                        FileRenameInfo,
                        buffer,
                        (uint)bufferLength))
                {
                    throw new IOException(
                        $"Handle-based release publication rename failed: {destinationPath}",
                        new Win32Exception(Marshal.GetLastWin32Error()));
                }
            }
            finally
            {
                Marshal.FreeHGlobal(buffer);
            }
        }

        private static void SetDeleteDisposition(SafeFileHandle handle)
        {
            var buffer = Marshal.AllocHGlobal(sizeof(int));
            try
            {
                Marshal.WriteInt32(buffer, 1);
                if (!SetFileInformationByHandle(
                        handle,
                        FileDispositionInfo,
                        buffer,
                        sizeof(int)))
                {
                    throw new IOException(
                        "Handle-based release deletion failed.",
                        new Win32Exception(Marshal.GetLastWin32Error()));
                }
            }
            finally
            {
                Marshal.FreeHGlobal(buffer);
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
                        "Unable to resolve owned release file identity.",
                        new Win32Exception(Marshal.GetLastWin32Error()));
                }
                if (length < capacity)
                {
                    return Normalize(buffer.ToString());
                }
                capacity = checked((int)length + 1);
            }
        }

        private static string Normalize(string path)
        {
            const string uncPrefix = @"\\?\UNC\";
            const string devicePrefix = @"\\?\";
            var value = path;
            if (value.StartsWith(uncPrefix, StringComparison.OrdinalIgnoreCase))
            {
                value = @"\\" + value.Substring(uncPrefix.Length);
            }
            else if (value.StartsWith(devicePrefix, StringComparison.OrdinalIgnoreCase))
            {
                value = value.Substring(devicePrefix.Length);
            }
            return System.IO.Path.GetFullPath(value);
        }

        private static string ToExtendedPath(string path)
        {
            if (path.StartsWith(@"\\?\", StringComparison.Ordinal))
            {
                return path;
            }
            if (path.StartsWith(@"\\", StringComparison.Ordinal))
            {
                return @"\\?\UNC\" + path.Substring(2);
            }
            return @"\\?\" + path;
        }

        private enum CreateDisposition : uint
        {
            CreateNew = 1,
            OpenExisting = 3
        }

        private const uint GenericRead = 0x80000000;
        private const uint GenericWrite = 0x40000000;
        private const uint DeleteAccess = 0x00010000;
        private const uint ShareRead = 0x00000001;
        private const uint SequentialScan = 0x08000000;
        private const uint OpenReparsePoint = 0x00200000;
        private const uint WriteThrough = 0x80000000;
        private const int FileRenameInfo = 3;
        private const int FileDispositionInfo = 4;

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

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool SetFileInformationByHandle(
            SafeFileHandle file,
            int fileInformationClass,
            IntPtr fileInformation,
            uint bufferSize);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern uint GetFinalPathNameByHandleW(
            SafeFileHandle file,
            StringBuilder filePath,
            uint filePathLength,
            uint flags);
    }

    public sealed class RedistributablePublicationLock : IDisposable
    {
        private FileStream stream;
        private bool disposed;

        private RedistributablePublicationLock(string path, FileStream stream)
        {
            Path = path;
            this.stream = stream;
        }

        public string Path { get; }

        public static RedistributablePublicationLock Acquire(string outputRoot)
        {
            var root = System.IO.Path.TrimEndingDirectorySeparator(
                System.IO.Path.GetFullPath(outputRoot));
            var lockPath = System.IO.Path.Combine(root, ".dsr-release-publication.lock");
            var handle = CreateFileW(
                lockPath,
                GenericRead | GenericWrite,
                0,
                IntPtr.Zero,
                OpenAlways,
                OpenReparsePoint | WriteThrough,
                IntPtr.Zero);
            if (handle.IsInvalid)
            {
                var error = Marshal.GetLastWin32Error();
                handle.Dispose();
                if (error == SharingViolation || error == LockViolation)
                {
                    throw new InvalidOperationException(
                        $"PUBLICATION_IN_PROGRESS: another publisher owns {root}");
                }
                throw new IOException(
                    $"Unable to acquire release publication lock: {lockPath}",
                    new Win32Exception(error));
            }

            FileStream lockStream = null;
            try
            {
                if (!GetFileInformationByHandle(handle, out var information))
                {
                    throw new IOException(
                        $"Unable to inspect release publication lock: {lockPath}",
                        new Win32Exception(Marshal.GetLastWin32Error()));
                }
                var attributes = (FileAttributes)information.FileAttributes;
                if ((attributes & FileAttributes.Directory) != 0
                    || (attributes & FileAttributes.ReparsePoint) != 0
                    || information.NumberOfLinks != 1)
                {
                    throw new UnauthorizedAccessException(
                        $"The release publication lock is unsafe: {lockPath}");
                }
                if (!ResolveFinalPath(handle).Equals(
                        System.IO.Path.GetFullPath(lockPath),
                        StringComparison.OrdinalIgnoreCase))
                {
                    throw new UnauthorizedAccessException(
                        $"The release publication lock resolves outside its path: {lockPath}");
                }
                lockStream = new FileStream(handle, FileAccess.ReadWrite, 4096, false);
                if (lockStream.Length != 0)
                {
                    throw new InvalidDataException(
                        $"The persistent release publication lock must be empty: {lockPath}");
                }
                return new RedistributablePublicationLock(lockPath, lockStream);
            }
            catch
            {
                if (lockStream != null)
                {
                    lockStream.Dispose();
                }
                else
                {
                    handle.Dispose();
                }
                throw;
            }
        }

        public void Dispose()
        {
            if (disposed)
            {
                return;
            }
            disposed = true;
            stream.Dispose();
            stream = null;
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
                        "Unable to resolve release publication lock identity.",
                        new Win32Exception(Marshal.GetLastWin32Error()));
                }
                if (length < capacity)
                {
                    var value = buffer.ToString();
                    if (value.StartsWith(@"\\?\UNC\", StringComparison.OrdinalIgnoreCase))
                    {
                        value = @"\\" + value.Substring(8);
                    }
                    else if (value.StartsWith(@"\\?\", StringComparison.OrdinalIgnoreCase))
                    {
                        value = value.Substring(4);
                    }
                    return System.IO.Path.GetFullPath(value);
                }
                capacity = checked((int)length + 1);
            }
        }

        private const uint GenericRead = 0x80000000;
        private const uint GenericWrite = 0x40000000;
        private const uint OpenAlways = 4;
        private const uint OpenReparsePoint = 0x00200000;
        private const uint WriteThrough = 0x80000000;
        private const int SharingViolation = 32;
        private const int LockViolation = 33;

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
'@
}

function Get-ReleaseSha256 {
    param([Parameter(Mandatory = $true)][IO.Stream]$Stream)

    if ($Stream.CanSeek) {
        $Stream.Position = 0
    }
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        ([Convert]::ToHexString($algorithm.ComputeHash($Stream))).ToLowerInvariant()
    }
    finally {
        $algorithm.Dispose()
        if ($Stream.CanSeek) {
            $Stream.Position = 0
        }
    }
}

function Invoke-RedistributableHook {
    param(
        [Parameter(Mandatory = $false)][scriptblock]$Hook,
        [Parameter(Mandatory = $true)][string]$Phase,
        [Parameter(Mandatory = $true)][string]$Path
    )
    if ($null -ne $Hook) {
        & $Hook $Phase $Path
    }
}

function Assert-RedistributableDescendant {
    param(
        [Parameter(Mandatory = $true)][string]$Candidate,
        [Parameter(Mandatory = $true)][string]$Root
    )
    $normalizedRoot = [IO.Path]::TrimEndingDirectorySeparator(
        [IO.Path]::GetFullPath($Root))
    $normalizedCandidate = [IO.Path]::GetFullPath($Candidate)
    if (-not $normalizedCandidate.StartsWith(
            $normalizedRoot + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Release path escaped its trusted root: $normalizedCandidate"
    }
}

function Assert-ReleaseRedistributableStream {
    param(
        [Parameter(Mandatory = $true)][IO.Stream]$Stream,
        [Parameter(Mandatory = $true)][string]$Description,
        [Parameter(Mandatory = $true)][string]$Version
    )

    $sourceName = "DSR-for-MOD-v$Version-source.zip"
    $binaryName = "DSR-for-MOD-v$Version-win-x64.zip"
    $expectedNames = @($sourceName, $binaryName, 'SHA256SUMS.txt')
    $zip = [IO.Compression.ZipArchive]::new(
        $Stream,
        [IO.Compression.ZipArchiveMode]::Read,
        $true)
    try {
        if ($zip.Entries.Count -ne $expectedNames.Count) {
            throw "Redistributable archive must contain exactly three entries: $Description"
        }
        for ($index = 0; $index -lt $expectedNames.Count; $index++) {
            $entry = $zip.Entries[$index]
            if ($entry.FullName -cne $expectedNames[$index]) {
                throw "Redistributable entry order/name is invalid at index $index."
            }
            if ($entry.FullName.Contains('/') `
                    -or $entry.FullName.Contains('\') `
                    -or $entry.FullName.Contains('..')) {
                throw "Redistributable entry path is unsafe: $($entry.FullName)"
            }
            if ($entry.LastWriteTime.DateTime -ne [datetime]'1980-01-01') {
                throw "Redistributable entry timestamp is not fixed: $($entry.FullName)"
            }
        }

        $sourceStream = $zip.Entries[0].Open()
        try {
            $sourceHash = Get-ReleaseSha256 -Stream $sourceStream
        }
        finally {
            $sourceStream.Dispose()
        }
        $binaryStream = $zip.Entries[1].Open()
        try {
            $binaryHash = Get-ReleaseSha256 -Stream $binaryStream
        }
        finally {
            $binaryStream.Dispose()
        }
        $sumsStream = $zip.Entries[2].Open()
        try {
            $memory = [IO.MemoryStream]::new()
            try {
                $sumsStream.CopyTo($memory)
                $sumsBytes = $memory.ToArray()
            }
            finally {
                $memory.Dispose()
            }
        }
        finally {
            $sumsStream.Dispose()
        }
        $expectedText = "$sourceHash  $sourceName`n$binaryHash  $binaryName`n"
        $expectedBytes = [Text.UTF8Encoding]::new($false).GetBytes($expectedText)
        if ([Convert]::ToHexString($sumsBytes) -cne [Convert]::ToHexString($expectedBytes)) {
            throw 'SHA256SUMS.txt does not strictly bind the two inner archives.'
        }
        [pscustomobject]@{
            ArchiveSha256 = Get-ReleaseSha256 -Stream $Stream
            SourceSha256 = $sourceHash
            BinarySha256 = $binaryHash
            Entries = $expectedNames
        }
    }
    finally {
        $zip.Dispose()
    }
}

function Assert-ReleaseRedistributableArchive {
    param(
        [Parameter(Mandatory = $true)][string]$ArchivePath,
        [Parameter(Mandatory = $true)][string]$Version
    )

    $lease = [DSRRandomizer.Packaging.RedistributableArtifactLease]::Acquire(
        $ArchivePath,
        'redistributable archive')
    try {
        Assert-ReleaseRedistributableStream `
            -Stream $lease.Stream `
            -Description $ArchivePath `
            -Version $Version
    }
    finally {
        $lease.Dispose()
    }
}

function New-ReleaseRedistributableArchive {
    param(
        [Parameter(Mandatory = $true)][string]$Version,
        [Parameter(Mandatory = $true)][string]$SourceArchivePath,
        [Parameter(Mandatory = $true)][string]$BinaryArchivePath,
        [Parameter(Mandatory = $true)][string]$OutputPath
    )

    $sourceName = "DSR-for-MOD-v$Version-source.zip"
    $binaryName = "DSR-for-MOD-v$Version-win-x64.zip"
    if ([IO.Path]::GetFileName($SourceArchivePath) -cne $sourceName `
            -or [IO.Path]::GetFileName($BinaryArchivePath) -cne $binaryName) {
        throw 'Inner archive names do not match the requested release version.'
    }
    $sourceLease = [DSRRandomizer.Packaging.RedistributableArtifactLease]::Acquire(
        $SourceArchivePath,
        'source archive')
    $binaryLease = $null
    try {
        $binaryLease = [DSRRandomizer.Packaging.RedistributableArtifactLease]::Acquire(
            $BinaryArchivePath,
            'binary archive')
        $sourceHash = $sourceLease.ComputeSha256()
        $binaryHash = $binaryLease.ComputeSha256()
        $output = [IO.FileStream]::new(
            [IO.Path]::GetFullPath($OutputPath),
            [IO.FileMode]::CreateNew,
            [IO.FileAccess]::ReadWrite,
            [IO.FileShare]::None,
            1024 * 1024,
            [IO.FileOptions]::WriteThrough)
        try {
            $zip = [IO.Compression.ZipArchive]::new(
                $output,
                [IO.Compression.ZipArchiveMode]::Create,
                $true)
            try {
                $timestamp = [DateTimeOffset]::new(
                    1980, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
                $sourceEntry = $zip.CreateEntry(
                    $sourceName,
                    [IO.Compression.CompressionLevel]::NoCompression)
                $sourceEntry.LastWriteTime = $timestamp
                $sourceOutput = $sourceEntry.Open()
                try {
                    $sourceLease.CopyTo($sourceOutput)
                }
                finally {
                    $sourceOutput.Dispose()
                }

                $binaryEntry = $zip.CreateEntry(
                    $binaryName,
                    [IO.Compression.CompressionLevel]::NoCompression)
                $binaryEntry.LastWriteTime = $timestamp
                $binaryOutput = $binaryEntry.Open()
                try {
                    $binaryLease.CopyTo($binaryOutput)
                }
                finally {
                    $binaryOutput.Dispose()
                }

                $sumsEntry = $zip.CreateEntry(
                    'SHA256SUMS.txt',
                    [IO.Compression.CompressionLevel]::NoCompression)
                $sumsEntry.LastWriteTime = $timestamp
                $sumsOutput = $sumsEntry.Open()
                try {
                    $text = "$sourceHash  $sourceName`n$binaryHash  $binaryName`n"
                    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($text)
                    $sumsOutput.Write($bytes, 0, $bytes.Length)
                }
                finally {
                    $sumsOutput.Dispose()
                }
            }
            finally {
                $zip.Dispose()
            }
            $output.Flush($true)
        }
        catch {
            $output.Dispose()
            if (Test-Path -LiteralPath $OutputPath -PathType Leaf) {
                [IO.File]::Delete([IO.Path]::GetFullPath($OutputPath))
            }
            throw
        }
        finally {
            $output.Dispose()
        }
    }
    finally {
        if ($null -ne $binaryLease) {
            $binaryLease.Dispose()
        }
        $sourceLease.Dispose()
    }
    Assert-ReleaseRedistributableArchive -ArchivePath $OutputPath -Version $Version | Out-Null
}

function Remove-ExactRegularFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Description
    )
    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    $owned = [DSRRandomizer.Packaging.RedistributableOwnedFile]::OpenExisting(
        $Path,
        $Description)
    try {
        $owned.DeleteOnClose()
    }
    finally {
        $owned.Dispose()
    }
}

function Publish-ReleaseRedistributableArchive {
    param(
        [Parameter(Mandatory = $true)][string]$StagedArchivePath,
        [Parameter(Mandatory = $true)][string]$OutputRoot,
        [Parameter(Mandatory = $true)][string]$FileName,
        [Parameter(Mandatory = $true)]
        [ValidatePattern('^[0-9a-f]{64}$')]
        [string]$ExpectedSha256,
        [Parameter(Mandatory = $true)][string]$Version,
        [Parameter(Mandatory = $false)][scriptblock]$OperationHook
    )

    $expectedName = "DSR-for-MOD-v$Version-redistributable.zip"
    if ($FileName -cne $expectedName -or [IO.Path]::GetFileName($FileName) -cne $FileName) {
        throw "Redistributable output name must be exact: $expectedName"
    }
    $root = Open-SafeReleaseRoot -Path $OutputRoot
    try {
        $lock = [DSRRandomizer.Packaging.RedistributablePublicationLock]::Acquire(
            $root.Path)
        try {
            Invoke-RedistributableHook `
                -Hook $OperationHook `
                -Phase 'AfterPublicationLockAcquired' `
                -Path $root.Path

            $finalPath = Join-Path $root.Path $FileName
            $pendingPath = Join-Path $root.Path (
                ".$FileName.pending-" + [Guid]::NewGuid().ToString('N'))
            $backupPath = "$finalPath.previous"
            Assert-RedistributableDescendant -Candidate $finalPath -Root $root.Path
            Assert-RedistributableDescendant -Candidate $pendingPath -Root $root.Path
            Assert-RedistributableDescendant -Candidate $backupPath -Root $root.Path

            if (Test-Path -LiteralPath $backupPath) {
                if (-not (Test-Path -LiteralPath $finalPath -PathType Leaf)) {
                    throw "A whole-archive backup exists without its canonical archive: $backupPath"
                }
                $staleBackup = [DSRRandomizer.Packaging.RedistributableOwnedFile]::OpenExisting(
                    $backupPath,
                    'previous redistributable backup')
                try {
                    Assert-ReleaseRedistributableStream `
                        -Stream $staleBackup.Stream `
                        -Description $backupPath `
                        -Version $Version | Out-Null
                    $staleBackup.DeleteOnClose()
                }
                finally {
                    $staleBackup.Dispose()
                }
            }

            $stagedLease = [DSRRandomizer.Packaging.RedistributableArtifactLease]::Acquire(
                $StagedArchivePath,
                'validated staged redistributable')
            $priorHash = $null
            $priorLease = $null
            $pendingOwned = $null
            $backupOwned = $null
            $finalOwned = $null
            $rollbackOwned = $null
            $replaced = $false
            try {
                if ($stagedLease.ComputeSha256() -cne $ExpectedSha256) {
                    throw 'The staged redistributable does not match its expected gated SHA-256.'
                }
                Assert-ReleaseRedistributableStream `
                    -Stream $stagedLease.Stream `
                    -Description $StagedArchivePath `
                    -Version $Version | Out-Null
                $pendingOwned = [DSRRandomizer.Packaging.RedistributableOwnedFile]::CreateFrom(
                    $stagedLease,
                    $pendingPath,
                    'pending redistributable')
                if ($pendingOwned.ComputeSha256() -cne $ExpectedSha256) {
                    throw 'The owned pending redistributable is not byte-exact.'
                }
                Assert-ReleaseRedistributableStream `
                    -Stream $pendingOwned.Stream `
                    -Description $pendingPath `
                    -Version $Version | Out-Null

                if (Test-Path -LiteralPath $finalPath -PathType Leaf) {
                    $priorLease = [DSRRandomizer.Packaging.RedistributableArtifactLease]::Acquire(
                        $finalPath,
                        'prior redistributable')
                    $priorHash = $priorLease.ComputeSha256()
                    Assert-ReleaseRedistributableStream `
                        -Stream $priorLease.Stream `
                        -Description $finalPath `
                        -Version $Version | Out-Null
                    $backupOwned = [DSRRandomizer.Packaging.RedistributableOwnedFile]::CreateFrom(
                        $priorLease,
                        $backupPath,
                        'whole redistributable backup')
                    if ($backupOwned.ComputeSha256() -cne $priorHash) {
                        throw 'The whole redistributable backup is not byte-exact.'
                    }
                    Assert-ReleaseRedistributableStream `
                        -Stream $backupOwned.Stream `
                        -Description $backupPath `
                        -Version $Version | Out-Null
                    $priorLease.Dispose()
                    $priorLease = $null
                }

                Invoke-RedistributableHook `
                    -Hook $OperationHook `
                    -Phase 'BeforeAtomicReplace' `
                    -Path $finalPath
                Invoke-RedistributableHook `
                    -Hook $OperationHook `
                    -Phase 'BeforeHandleRename' `
                    -Path $pendingPath
                $pendingOwned.RenameTo($finalPath, $true)
                $finalOwned = $pendingOwned
                $pendingOwned = $null
                $replaced = $true
                Invoke-RedistributableHook `
                    -Hook $OperationHook `
                    -Phase 'BeforeFinalVerification' `
                    -Path $finalPath
                if ($finalOwned.ComputeSha256() -cne $ExpectedSha256) {
                    throw 'Published redistributable SHA-256 does not match the gated input.'
                }
                Assert-ReleaseRedistributableStream `
                    -Stream $finalOwned.Stream `
                    -Description $finalPath `
                    -Version $Version | Out-Null
                if ($null -ne $backupOwned) {
                    try {
                        Invoke-RedistributableHook `
                            -Hook $OperationHook `
                            -Phase 'BeforeBackupCleanup' `
                            -Path $backupPath
                        $backupOwned.DeleteOnClose()
                        $backupOwned.Dispose()
                        $backupOwned = $null
                    }
                    catch {
                        Write-Warning (
                            'Published redistributable is valid, but its whole-file ' +
                            "backup could not be removed: $backupPath. $($_.Exception.Message)")
                    }
                }
            }
            catch {
                $publicationError = $_
                if ($null -ne $priorLease) {
                    $priorLease.Dispose()
                    $priorLease = $null
                }
                if ($replaced) {
                    try {
                        $failedPath = Join-Path $root.Path (
                            ".$FileName.failed-" + [Guid]::NewGuid().ToString('N'))
                        Assert-RedistributableDescendant `
                            -Candidate $failedPath `
                            -Root $root.Path
                        $finalOwned.RenameTo($failedPath, $false)
                        $finalOwned.DeleteOnClose()
                        if ($null -ne $backupOwned) {
                            $rollbackPath = Join-Path $root.Path (
                                ".$FileName.rollback-" + [Guid]::NewGuid().ToString('N'))
                            Assert-RedistributableDescendant `
                                -Candidate $rollbackPath `
                                -Root $root.Path
                            $rollbackOwned = [DSRRandomizer.Packaging.RedistributableOwnedFile]::CreateFrom(
                                $backupOwned,
                                $rollbackPath,
                                'rollback redistributable candidate')
                            if ($rollbackOwned.ComputeSha256() -cne $priorHash) {
                                throw 'Rollback candidate does not match the leased prior bytes.'
                            }
                            Assert-ReleaseRedistributableStream `
                                -Stream $rollbackOwned.Stream `
                                -Description $rollbackPath `
                                -Version $Version | Out-Null
                            Invoke-RedistributableHook `
                                -Hook $OperationHook `
                                -Phase 'BeforeRollback' `
                                -Path $backupPath
                            $rollbackOwned.RenameTo($finalPath, $false)
                            if ($rollbackOwned.ComputeSha256() -cne $priorHash) {
                                throw 'Restored redistributable does not match the leased prior bytes.'
                            }
                            Assert-ReleaseRedistributableStream `
                                -Stream $rollbackOwned.Stream `
                                -Description $finalPath `
                                -Version $Version | Out-Null
                            try {
                                $backupOwned.DeleteOnClose()
                                $backupOwned.Dispose()
                                $backupOwned = $null
                            }
                            catch {
                                Write-Warning (
                                    'Rollback restored the prior redistributable, but its ' +
                                    "whole-file backup remains: $backupPath. $($_.Exception.Message)")
                            }
                        }
                    }
                    catch {
                        throw [AggregateException]::new(
                            'Redistributable publication and handle-owned rollback both failed.',
                            @($publicationError.Exception, $_.Exception))
                    }
                }
                elseif ($null -ne $backupOwned) {
                    try {
                        $backupOwned.DeleteOnClose()
                        $backupOwned.Dispose()
                        $backupOwned = $null
                    }
                    catch {
                        Write-Warning (
                            'Unused whole-file backup could not be removed after a ' +
                            "pre-replace failure: $backupPath. $($_.Exception.Message)")
                    }
                }
                throw $publicationError
            }
            finally {
                if ($null -ne $priorLease) {
                    $priorLease.Dispose()
                }
                if ($null -ne $pendingOwned) {
                    try {
                        $pendingOwned.DeleteOnClose()
                    }
                    finally {
                        $pendingOwned.Dispose()
                    }
                }
                if ($null -ne $backupOwned) {
                    $backupOwned.Dispose()
                }
                if ($null -ne $finalOwned) {
                    $finalOwned.Dispose()
                }
                if ($null -ne $rollbackOwned) {
                    $rollbackOwned.Dispose()
                }
                $stagedLease.Dispose()
            }
        }
        finally {
            $lock.Dispose()
        }
    }
    finally {
        $root.Lease.Dispose()
    }
}

function Remove-LegacyReleaseArtifacts {
    param(
        [Parameter(Mandatory = $true)][string]$OutputRoot,
        [Parameter(Mandatory = $true)]
        [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z]+(?:\.[0-9A-Za-z]+)*)?$')]
        [string]$Version
    )

    $names = @(
        "DSR-for-MOD-v$Version-win-x64.zip",
        "DSR-for-MOD-v$Version-win-x64.zip.sha256",
        "DSR-for-MOD-v$Version-source.zip",
        "DSR-for-MOD-v$Version-source.zip.sha256"
    )
    $root = Open-SafeReleaseRoot -Path $OutputRoot
    try {
        $lock = [DSRRandomizer.Packaging.RedistributablePublicationLock]::Acquire(
            $root.Path)
        try {
            foreach ($name in $names) {
                $path = Join-Path $root.Path $name
                Assert-RedistributableDescendant -Candidate $path -Root $root.Path
                try {
                    Remove-ExactRegularFile `
                        -Path $path `
                        -Description 'legacy release artifact'
                }
                catch {
                    Write-Warning "Legacy artifact was left in place: $path. $($_.Exception.Message)"
                }
            }
        }
        finally {
            $lock.Dispose()
        }
    }
    finally {
        $root.Lease.Dispose()
    }
}

Export-ModuleMember -Function `
    New-ReleaseRedistributableArchive, `
    Assert-ReleaseRedistributableArchive, `
    Publish-ReleaseRedistributableArchive, `
    Remove-LegacyReleaseArtifacts
