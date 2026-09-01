Set-StrictMode -Version Latest

if ($null -eq ('DSRRandomizer.Packaging.SafeDirectoryLease' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace DSRRandomizer.Packaging
{
    public sealed class SafeDirectoryLease : IDisposable
    {
        private readonly List<SafeFileHandle> handles;
        private FileStream identityLock;
        private bool disposed;

        private SafeDirectoryLease(
            string path,
            List<SafeFileHandle> handles,
            FileStream identityLock)
        {
            Path = path;
            this.handles = handles;
            this.identityLock = identityLock;
        }

        public string Path { get; }

        public static SafeDirectoryLease Acquire(string path)
        {
            if (string.IsNullOrWhiteSpace(path))
            {
                throw new ArgumentException("A release directory path is required.", nameof(path));
            }

            var fullPath = System.IO.Path.GetFullPath(path);
            if (!Directory.Exists(fullPath))
            {
                throw new DirectoryNotFoundException($"Release directory does not exist: {fullPath}");
            }

            var root = System.IO.Path.GetPathRoot(fullPath)
                ?? throw new IOException($"Release directory root is invalid: {fullPath}");
            if (System.IO.Path.TrimEndingDirectorySeparator(fullPath).Equals(
                System.IO.Path.TrimEndingDirectorySeparator(root),
                StringComparison.OrdinalIgnoreCase))
            {
                throw new UnauthorizedAccessException(
                    $"A volume root cannot be used as a release directory: {fullPath}");
            }
            var relative = System.IO.Path.GetRelativePath(root, fullPath);
            var paths = new List<string> { root };
            var current = root;
            if (!relative.Equals(".", StringComparison.Ordinal))
            {
                foreach (var segment in relative.Split(
                    System.IO.Path.DirectorySeparatorChar,
                    StringSplitOptions.RemoveEmptyEntries))
                {
                    current = System.IO.Path.Combine(current, segment);
                    paths.Add(current);
                }
            }

            var handles = new List<SafeFileHandle>(paths.Count);
            FileStream identityLock = null;
            try
            {
                foreach (var candidate in paths)
                {
                    handles.Add(OpenVerifiedDirectory(candidate));
                }
                var lockPath = System.IO.Path.TrimEndingDirectorySeparator(fullPath)
                    + ":dsr-randomizer-release-lease-"
                    + Guid.NewGuid().ToString("N");
                identityLock = new FileStream(lockPath, new FileStreamOptions
                {
                    Access = FileAccess.ReadWrite,
                    Mode = FileMode.CreateNew,
                    Share = FileShare.None,
                    Options = FileOptions.DeleteOnClose
                });
                return new SafeDirectoryLease(fullPath, handles, identityLock);
            }
            catch
            {
                identityLock?.Dispose();
                DisposeHandles(handles);
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
            identityLock.Dispose();
            identityLock = null;
            DisposeHandles(handles);
        }

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
                    $"Unable to lease release directory: {path}",
                    new Win32Exception(error));
            }

            try
            {
                if (!GetFileInformationByHandle(handle, out var information))
                {
                    throw new IOException(
                        $"Unable to inspect release directory: {path}",
                        new Win32Exception(Marshal.GetLastWin32Error()));
                }
                var attributes = (FileAttributes)information.FileAttributes;
                if ((attributes & FileAttributes.Directory) == 0
                    || (attributes & FileAttributes.ReparsePoint) != 0)
                {
                    throw new UnauthorizedAccessException(
                        $"Release directory is a reparse point or not a regular directory: {path}");
                }

                var finalPath = ResolveFinalPath(handle);
                var lexicalPath = Normalize(path);
                if (!finalPath.Equals(lexicalPath, StringComparison.OrdinalIgnoreCase))
                {
                    throw new UnauthorizedAccessException(
                        $"Release directory resolves outside its lexical path: {path}");
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
                        "Unable to resolve release directory identity.",
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
            return System.IO.Path.GetFullPath(value)
                .TrimEnd(System.IO.Path.DirectorySeparatorChar);
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
'@
}

function Assert-StrictReleaseDescendant {
    param(
        [Parameter(Mandatory = $true)][string]$Candidate,
        [Parameter(Mandatory = $true)][string]$Root
    )

    $normalizedRoot = [IO.Path]::TrimEndingDirectorySeparator([IO.Path]::GetFullPath($Root))
    $normalizedCandidate = [IO.Path]::GetFullPath($Candidate)
    if (-not $normalizedCandidate.StartsWith(
        $normalizedRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
        throw "Release directory escaped its trusted root: $normalizedCandidate"
    }
}

function Open-SafeReleaseRoot {
    param([Parameter(Mandatory = $true)][string]$Path)

    $fullPath = [IO.Path]::GetFullPath($Path)
    $missing = [Collections.Generic.Stack[string]]::new()
    $existing = $fullPath
    while (-not (Test-Path -LiteralPath $existing -PathType Container)) {
        if (Test-Path -LiteralPath $existing) {
            throw "A release directory path is occupied by a non-directory: $existing"
        }
        $trimmed = [IO.Path]::TrimEndingDirectorySeparator($existing)
        $leaf = [IO.Path]::GetFileName($trimmed)
        $parent = [IO.Path]::GetDirectoryName($trimmed)
        if ([string]::IsNullOrEmpty($leaf) -or [string]::IsNullOrEmpty($parent)) {
            throw "No existing trusted ancestor was found for release root: $fullPath"
        }
        $missing.Push($leaf)
        $existing = $parent
    }

    $lease = [DSRRandomizer.Packaging.SafeDirectoryLease]::Acquire($existing)
    try {
        while ($missing.Count -ne 0) {
            $next = Join-Path $existing $missing.Pop()
            New-Item -ItemType Directory -Path $next -ErrorAction Stop | Out-Null
            $nextLease = [DSRRandomizer.Packaging.SafeDirectoryLease]::Acquire($next)
            $lease.Dispose()
            $lease = $nextLease
            $existing = $next
        }
        [pscustomobject]@{
            Path = $fullPath
            Lease = $lease
        }
    }
    catch {
        $lease.Dispose()
        throw
    }
}

function New-SafeReleaseDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$TrustedRoot,
        [Parameter(Mandatory = $true)]
        [ValidatePattern('^[0-9A-Za-z][0-9A-Za-z.-]*-$')]
        [string]$LeafPrefix
    )

    $rootLease = [DSRRandomizer.Packaging.SafeDirectoryLease]::Acquire($TrustedRoot)
    $generatedPath = Join-Path $rootLease.Path (
        $LeafPrefix + [Guid]::NewGuid().ToString('N'))
    Assert-StrictReleaseDescendant -Candidate $generatedPath -Root $rootLease.Path
    try {
        New-Item -ItemType Directory -Path $generatedPath -ErrorAction Stop | Out-Null
        $generatedLease = [DSRRandomizer.Packaging.SafeDirectoryLease]::Acquire($generatedPath)
        [pscustomobject]@{
            Path = $generatedPath
            Root = $rootLease.Path
            LeafPrefix = $LeafPrefix
            Lease = $generatedLease
        }
    }
    catch {
        if (Test-Path -LiteralPath $generatedPath -PathType Container) {
            $attributes = [IO.File]::GetAttributes($generatedPath)
            if (($attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0 `
                -and @(Get-ChildItem -LiteralPath $generatedPath -Force).Count -eq 0) {
                [IO.Directory]::Delete($generatedPath)
            }
        }
        throw
    }
    finally {
        $rootLease.Dispose()
    }
}

function Assert-NoReleaseReparseTree {
    param([Parameter(Mandatory = $true)][string]$Path)

    $attributes = [IO.File]::GetAttributes($Path)
    if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Release cleanup rejected a reparse point: $Path"
    }
    foreach ($entry in Get-ChildItem -LiteralPath $Path -Force) {
        if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Release cleanup rejected a reparse point: $($entry.FullName)"
        }
        if (($entry.Attributes -band [IO.FileAttributes]::Directory) -ne 0) {
            $childLease = [DSRRandomizer.Packaging.SafeDirectoryLease]::Acquire(
                $entry.FullName)
            try {
                Assert-NoReleaseReparseTree -Path $entry.FullName
            }
            finally {
                $childLease.Dispose()
            }
        }
    }
}

function Clear-RegularReleaseDirectory {
    param([Parameter(Mandatory = $true)][string]$Path)

    foreach ($entry in Get-ChildItem -LiteralPath $Path -Force) {
        if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Release cleanup rejected a reparse point: $($entry.FullName)"
        }
        if (($entry.Attributes -band [IO.FileAttributes]::Directory) -ne 0) {
            $childLease = [DSRRandomizer.Packaging.SafeDirectoryLease]::Acquire(
                $entry.FullName)
            try {
                Clear-RegularReleaseDirectory -Path $entry.FullName
            }
            finally {
                $childLease.Dispose()
            }
            [IO.Directory]::Delete($entry.FullName)
        }
        else {
            [IO.File]::Delete($entry.FullName)
        }
    }
}

function Remove-SafeReleaseDirectory {
    param([Parameter(Mandatory = $true)][object]$Directory)

    $path = [IO.Path]::GetFullPath([string]$Directory.Path)
    $root = [IO.Path]::GetFullPath([string]$Directory.Root)
    $leafPrefix = [string]$Directory.LeafPrefix
    Assert-StrictReleaseDescendant -Candidate $path -Root $root
    if (-not [IO.Path]::GetFileName($path).StartsWith(
        $leafPrefix,
        [StringComparison]::Ordinal)) {
        throw "Release cleanup target has an unexpected leaf: $path"
    }
    if (-not $Directory.Lease.Path.Equals($path, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Release cleanup lease does not match its target: $path"
    }

    Assert-NoReleaseReparseTree -Path $path
    Clear-RegularReleaseDirectory -Path $path
    $Directory.Lease.Dispose()
    $Directory.Lease = $null
    [IO.Directory]::Delete($path)
}

Export-ModuleMember -Function `
    Open-SafeReleaseRoot, `
    New-SafeReleaseDirectory, `
    Remove-SafeReleaseDirectory
