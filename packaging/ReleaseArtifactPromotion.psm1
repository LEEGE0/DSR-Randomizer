Set-StrictMode -Version Latest

Import-Module (Join-Path $PSScriptRoot 'SafeReleaseDirectories.psm1')

if ($null -eq ('DSRRandomizer.Packaging.ReleaseArtifactLease' -as [type])) {
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
    public sealed class ReleaseArtifactLease : IDisposable
    {
        private FileStream stream;
        private bool disposed;

        private ReleaseArtifactLease(string path, FileStream stream, string identity)
        {
            Path = path;
            this.stream = stream;
            Identity = identity;
        }

        public string Path { get; }
        public string Identity { get; }
        public long Length => stream.Length;

        public static ReleaseArtifactLease Acquire(string path, string description)
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
                var lexicalPath = Normalize(fullPath);
                if (!finalPath.Equals(lexicalPath, StringComparison.OrdinalIgnoreCase))
                {
                    throw new UnauthorizedAccessException(
                        $"The {description} resolves outside its lexical path: {fullPath}");
                }

                var identity = information.VolumeSerialNumber.ToString("x8")
                    + ":"
                    + information.FileIndexHigh.ToString("x8")
                    + information.FileIndexLow.ToString("x8");
                leasedStream = new FileStream(handle, FileAccess.Read, 1024 * 1024, false);
                return new ReleaseArtifactLease(fullPath, leasedStream, identity);
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
            var digest = SHA256.HashData(stream);
            stream.Position = 0;
            return Convert.ToHexString(digest).ToLowerInvariant();
        }

        public byte[] ReadAllBytes(int maximumLength)
        {
            ThrowIfDisposed();
            if (stream.Length > maximumLength)
            {
                throw new InvalidDataException(
                    $"Leased artifact exceeds the maximum expected length: {Path}");
            }
            stream.Position = 0;
            using var memory = new MemoryStream((int)stream.Length);
            stream.CopyTo(memory);
            stream.Position = 0;
            return memory.ToArray();
        }

        public void CopyToNewAndFlush(string destinationPath)
        {
            ThrowIfDisposed();
            stream.Position = 0;
            using (var destination = new FileStream(destinationPath, new FileStreamOptions
            {
                Access = FileAccess.Write,
                Mode = FileMode.CreateNew,
                Share = FileShare.None,
                Options = FileOptions.WriteThrough,
                BufferSize = 1024 * 1024
            }))
            {
                stream.CopyTo(destination, 1024 * 1024);
                destination.Flush(true);
            }
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
                throw new ObjectDisposedException(nameof(ReleaseArtifactLease));
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

    public sealed class ReleasePublicationLock : IDisposable
    {
        private FileStream stream;
        private bool disposed;

        private ReleasePublicationLock(string path, FileStream stream)
        {
            Path = path;
            this.stream = stream;
        }

        public string Path { get; }

        public static ReleasePublicationLock Acquire(string outputRoot)
        {
            var canonicalRoot = System.IO.Path.TrimEndingDirectorySeparator(
                System.IO.Path.GetFullPath(outputRoot));
            var lockPath = System.IO.Path.Combine(
                canonicalRoot,
                ".dsr-release-publication.lock");
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
                        $"PUBLICATION_IN_PROGRESS: another release publication owns the output root lock: {canonicalRoot}");
                }
                throw new IOException(
                    $"Unable to acquire the release publication lock: {lockPath}",
                    new Win32Exception(error));
            }

            FileStream lockStream = null;
            try
            {
                if (!GetFileInformationByHandle(handle, out var information))
                {
                    throw new IOException(
                        $"Unable to inspect the release publication lock: {lockPath}",
                        new Win32Exception(Marshal.GetLastWin32Error()));
                }
                var attributes = (FileAttributes)information.FileAttributes;
                if ((attributes & FileAttributes.Directory) != 0
                    || (attributes & FileAttributes.ReparsePoint) != 0)
                {
                    throw new UnauthorizedAccessException(
                        $"The release publication lock is a reparse point or not a regular file: {lockPath}");
                }
                if (information.NumberOfLinks != 1)
                {
                    throw new UnauthorizedAccessException(
                        $"The release publication lock has multiple hard links: {lockPath}");
                }
                var finalPath = ResolveFinalPath(handle);
                if (!finalPath.Equals(
                        System.IO.Path.GetFullPath(lockPath),
                        StringComparison.OrdinalIgnoreCase))
                {
                    throw new UnauthorizedAccessException(
                        $"The release publication lock resolves outside its lexical path: {lockPath}");
                }

                lockStream = new FileStream(handle, FileAccess.ReadWrite, 4096, false);
                if (lockStream.Length != 0)
                {
                    throw new InvalidDataException(
                        $"The persistent release publication lock must be empty: {lockPath}");
                }
                return new ReleasePublicationLock(lockPath, lockStream);
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
                        "Unable to resolve the release publication lock identity.",
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

    public static class ReleaseJournalPersistence
    {
        public static void ReplaceWriteThrough(string sourcePath, string destinationPath)
        {
            var source = System.IO.Path.GetFullPath(sourcePath);
            var destination = System.IO.Path.GetFullPath(destinationPath);
            if (!System.IO.Path.GetDirectoryName(source).Equals(
                    System.IO.Path.GetDirectoryName(destination),
                    StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidOperationException(
                    "Release journal replacement must stay within one transaction directory.");
            }
            if (!MoveFileExW(
                    ToExtendedPath(source),
                    ToExtendedPath(destination),
                    MoveFileReplaceExisting | MoveFileWriteThrough))
            {
                throw new IOException(
                    $"Durable release transaction journal replacement failed: {destination}",
                    new Win32Exception(Marshal.GetLastWin32Error()));
            }
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

        private const uint MoveFileReplaceExisting = 0x00000001;
        private const uint MoveFileWriteThrough = 0x00000008;

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool MoveFileExW(
            string existingFileName,
            string newFileName,
            uint flags);
    }
}
'@
}

function Assert-RegularArtifactFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Description
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description is missing: $Path"
    }
    $attributes = [IO.File]::GetAttributes($Path)
    if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Description is a reparse point: $Path"
    }
}

function Set-ReleaseTransactionState {
    param(
        [Parameter(Mandatory = $true)][string]$TransactionRoot,
        [Parameter(Mandatory = $true)][Collections.IDictionary]$State,
        [Parameter(Mandatory = $false)][scriptblock]$OperationHook,
        [Parameter(Mandatory = $false)][Collections.IDictionary]$FallbackState
    )

    $statePath = Join-Path $TransactionRoot 'transaction-state.json'
    $temporaryStatePath = Join-Path $TransactionRoot (
        'transaction-state.' + [Guid]::NewGuid().ToString('N') + '.next')
    $json = $State | ConvertTo-Json -Compress -Depth 4
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($json)
    $phase = [string]$State.phase
    try {
        $stream = [IO.FileStream]::new(
            $temporaryStatePath,
            [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write,
            [IO.FileShare]::None,
            4096,
            [IO.FileOptions]::WriteThrough)
        try {
            $stream.Write($bytes, 0, $bytes.Length)
            $stream.Flush($true)
        }
        finally {
            $stream.Dispose()
        }
        $journalLease = [DSRRandomizer.Packaging.ReleaseArtifactLease]::Acquire(
            $temporaryStatePath,
            'temporary release transaction journal')
        $journalLease.Dispose()
        Invoke-PromotionHook `
            -OperationHook $OperationHook `
            -Phase 'BeforeJournalMove' `
            -Index -1 `
            -Name $phase
        try {
            [DSRRandomizer.Packaging.ReleaseJournalPersistence]::ReplaceWriteThrough(
                $temporaryStatePath,
                $statePath)
        }
        catch {
            try {
                Invoke-PromotionHook `
                    -OperationHook $OperationHook `
                    -Phase 'AfterJournalMoveFailure' `
                    -Index -1 `
                    -Name $phase
            }
            catch {
                # Preserve the durable-move failure as the primary publication error.
            }
            throw
        }

        try {
            Invoke-PromotionHook `
                -OperationHook $OperationHook `
                -Phase 'BeforeJournalVerification' `
                -Index -1 `
                -Name $phase
            $liveLease = [DSRRandomizer.Packaging.ReleaseArtifactLease]::Acquire(
                $statePath,
                'durably replaced release transaction journal')
            try {
                $expectedHash = [Convert]::ToHexString(
                    [Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
                if ($liveLease.ComputeSha256() -cne $expectedHash) {
                    throw 'Durably replaced release transaction journal bytes do not match the requested state.'
                }
            }
            finally {
                $liveLease.Dispose()
            }
            $verifiedState = Read-ReleaseTransactionState `
                -TransactionRoot $TransactionRoot `
                -Names @($State.artifactNames)
            if ($verifiedState.Phase -cne $phase) {
                throw "Durably replaced release transaction journal did not verify phase '$phase'."
            }
            return $verifiedState
        }
        catch {
            $verificationFailure = $_.Exception
            if ($null -ne $FallbackState) {
                try {
                    Set-ReleaseTransactionState `
                        -TransactionRoot $TransactionRoot `
                        -State $FallbackState | Out-Null
                }
                catch {
                    throw [IO.IOException]::new(
                        "Release transaction journal verification failed and durable Prepared fallback also failed. Verification failure: $($verificationFailure.Message) Fallback failure: $($_.Exception.Message)",
                        $verificationFailure)
                }
            }
            throw $verificationFailure
        }
    }
    finally {
        if (Test-Path -LiteralPath $temporaryStatePath -PathType Leaf) {
            Assert-RegularArtifactFile `
                -Path $temporaryStatePath `
                -Description 'Temporary release transaction journal'
            [IO.File]::Delete($temporaryStatePath)
        }
    }
}

function Invoke-PromotionHook {
    param(
        [Parameter(Mandatory = $false)][scriptblock]$OperationHook,
        [Parameter(Mandatory = $true)][string]$Phase,
        [Parameter(Mandatory = $true)][int]$Index,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Name
    )

    if ($null -ne $OperationHook) {
        & $OperationHook $Phase $Index $Name
    }
}

function Close-ReleaseArtifactLeases {
    param([Parameter(Mandatory = $true)][Collections.IList]$Leases)

    for ($index = $Leases.Count - 1; $index -ge 0; $index--) {
        $Leases[$index].Dispose()
    }
    $Leases.Clear()
}

function Get-ValidatedExpectedArchiveHashes {
    param(
        [Parameter(Mandatory = $true)][string[]]$Names,
        [Parameter(Mandatory = $true)][Collections.IDictionary]$ExpectedArchiveHashes
    )

    $archiveNames = @($Names[0], $Names[2])
    if (-not $Names[0].EndsWith('.zip', [StringComparison]::Ordinal) `
            -or $Names[1] -cne "$($Names[0]).sha256" `
            -or -not $Names[2].EndsWith('.zip', [StringComparison]::Ordinal) `
            -or $Names[3] -cne "$($Names[2]).sha256") {
        throw 'Release artifact names must be ordered as ZIP, matching sidecar, ZIP, matching sidecar.'
    }
    if ($ExpectedArchiveHashes.Count -ne 2) {
        throw 'Expected archive hashes must contain exactly the two release ZIP names.'
    }

    $validated = [ordered]@{}
    foreach ($archiveName in $archiveNames) {
        if (-not $ExpectedArchiveHashes.Contains($archiveName)) {
            throw "Expected archive hash is missing for: $archiveName"
        }
        $hash = [string]$ExpectedArchiveHashes[$archiveName]
        if ($hash -cnotmatch '^[0-9a-f]{64}$') {
            throw "Expected archive hash must be lowercase SHA-256 for: $archiveName"
        }
        $validated[$archiveName] = $hash
    }
    foreach ($key in $ExpectedArchiveHashes.Keys) {
        if ($archiveNames -cnotcontains [string]$key) {
            throw "Expected archive hashes contain an unexpected name: $key"
        }
    }
    return $validated
}

function Assert-LeasedArtifactSet {
    param(
        [Parameter(Mandatory = $true)][string[]]$Names,
        [Parameter(Mandatory = $true)][Collections.IList]$Leases,
        [Parameter(Mandatory = $true)][Collections.IDictionary]$ExpectedArchiveHashes,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $blobHashes = [ordered]@{}
    for ($index = 0; $index -lt $Names.Count; $index++) {
        $blobHashes[$Names[$index]] = $Leases[$index].ComputeSha256()
    }

    foreach ($archiveIndex in @(0, 2)) {
        $archiveName = $Names[$archiveIndex]
        $actualHash = $blobHashes[$archiveName]
        if ($actualHash -cne $ExpectedArchiveHashes[$archiveName]) {
            throw "$Description archive '$archiveName' does not match its expected gated SHA-256."
        }

        $sidecarName = $Names[$archiveIndex + 1]
        $expectedSidecarBytes = [Text.UTF8Encoding]::new($false).GetBytes(
            "$actualHash  $archiveName`n")
        $actualSidecarBytes = $Leases[$archiveIndex + 1].ReadAllBytes(512)
        if ([Convert]::ToHexString($actualSidecarBytes) -cne `
                [Convert]::ToHexString($expectedSidecarBytes)) {
            throw "$Description checksum sidecar does not exactly match '$archiveName'."
        }
    }
    return $blobHashes
}

function Assert-BlobHashesEqual {
    param(
        [Parameter(Mandatory = $true)][string[]]$Names,
        [Parameter(Mandatory = $true)][Collections.IDictionary]$Actual,
        [Parameter(Mandatory = $true)][Collections.IDictionary]$Expected,
        [Parameter(Mandatory = $true)][string]$Description
    )

    foreach ($name in $Names) {
        if (-not $Expected.Contains($name) `
                -or [string]$Actual[$name] -cne [string]$Expected[$name]) {
            throw "$Description blob hash does not match for '$name'."
        }
    }
}

function Open-ReleaseArtifactSetLeases {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string[]]$Names,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $leases = [Collections.Generic.List[object]]::new()
    try {
        foreach ($name in $Names) {
            $leases.Add(
                [DSRRandomizer.Packaging.ReleaseArtifactLease]::Acquire(
                    (Join-Path $Root $name),
                    $Description))
        }
        return ,$leases
    }
    catch {
        Close-ReleaseArtifactLeases -Leases $leases
        throw
    }
}

function Get-ArchiveHashesFromLeases {
    param(
        [Parameter(Mandatory = $true)][string[]]$Names,
        [Parameter(Mandatory = $true)][Collections.IList]$Leases
    )

    return [ordered]@{
        $Names[0] = $Leases[0].ComputeSha256()
        $Names[2] = $Leases[2].ComputeSha256()
    }
}

function Convert-StateHashMap {
    param(
        [Parameter(Mandatory = $true)][object]$Map,
        [Parameter(Mandatory = $true)][string[]]$ExpectedNames,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $properties = @($Map.PSObject.Properties)
    if ($properties.Count -ne $ExpectedNames.Count) {
        throw "$Description must contain exactly $($ExpectedNames.Count) hashes."
    }
    $result = [ordered]@{}
    foreach ($name in $ExpectedNames) {
        $property = @($properties | Where-Object { $_.Name -ceq $name })
        if ($property.Count -ne 1) {
            throw "$Description is missing '$name'."
        }
        $value = [string]$property[0].Value
        if ($value -cnotmatch '^[0-9a-f]{64}$') {
            throw "$Description contains an invalid lowercase SHA-256 for '$name'."
        }
        $result[$name] = $value
    }
    return $result
}

function Read-ReleaseTransactionState {
    param(
        [Parameter(Mandatory = $true)][string]$TransactionRoot,
        [Parameter(Mandatory = $true)][string[]]$Names
    )

    $statePath = Join-Path $TransactionRoot 'transaction-state.json'
    $lease = [DSRRandomizer.Packaging.ReleaseArtifactLease]::Acquire(
        $statePath,
        'release transaction journal')
    try {
        $json = [Text.UTF8Encoding]::new($false, $true).GetString(
            $lease.ReadAllBytes(65536))
    }
    finally {
        $lease.Dispose()
    }
    try {
        $state = $json | ConvertFrom-Json -ErrorAction Stop
    }
    catch {
        throw [IO.InvalidDataException]::new(
            "Release transaction journal is not valid JSON: $statePath",
            $_.Exception)
    }

    $expectedProperties = @(
        'schemaVersion',
        'phase',
        'hadPriorSet',
        'artifactNames',
        'expectedArchiveHashes',
        'expectedFinalBlobHashes',
        'backupBlobHashes',
        'progress',
        'promotedIdentities')
    $actualProperties = @($state.PSObject.Properties.Name)
    if ($actualProperties.Count -ne $expectedProperties.Count `
            -or @(Compare-Object $expectedProperties $actualProperties -CaseSensitive).Count -ne 0) {
        throw 'Release transaction journal has an unexpected schema.'
    }
    if ([int]$state.schemaVersion -ne 4 `
            -or @('Prepared', 'Committed') -cnotcontains [string]$state.phase `
            -or $state.hadPriorSet -isnot [bool]) {
        throw 'Release transaction journal has invalid version, phase, or prior-set state.'
    }
    $journalNames = @($state.artifactNames)
    if ($journalNames.Count -ne $Names.Count) {
        throw 'Release transaction journal artifact names do not match this publication.'
    }
    for ($index = 0; $index -lt $Names.Count; $index++) {
        if ([string]$journalNames[$index] -cne $Names[$index]) {
            throw 'Release transaction journal artifact names do not match this publication.'
        }
    }

    $archiveNames = @($Names[0], $Names[2])
    $expectedArchiveHashes = Convert-StateHashMap `
        -Map $state.expectedArchiveHashes `
        -ExpectedNames $archiveNames `
        -Description 'Journal expected archive hashes'
    $expectedFinalBlobHashes = Convert-StateHashMap `
        -Map $state.expectedFinalBlobHashes `
        -ExpectedNames $Names `
        -Description 'Journal expected final blob hashes'
    $backupProperties = @($state.backupBlobHashes.PSObject.Properties)
    if ([bool]$state.hadPriorSet) {
        $backupBlobHashes = Convert-StateHashMap `
            -Map $state.backupBlobHashes `
            -ExpectedNames $Names `
            -Description 'Journal backup blob hashes'
    }
    else {
        if ($backupProperties.Count -ne 0) {
            throw 'A no-prior-set transaction journal contains unexpected backup hashes.'
        }
        $backupBlobHashes = [ordered]@{}
    }
    $progress = [string]$state.progress
    if (@(
            'BackupsVerified',
            'PriorRemoved',
            'Publishing',
            'FinalsWritten',
            'FinalVerified') -cnotcontains $progress) {
        throw 'Release transaction journal has invalid output progress.'
    }
    $promotedIdentities = @($state.promotedIdentities)
    if ($promotedIdentities.Count -ne $Names.Count) {
        throw 'Release transaction journal has an invalid promoted identity count.'
    }
    for ($index = 0; $index -lt $promotedIdentities.Count; $index++) {
        $identity = [string]$promotedIdentities[$index]
        if ($identity.Length -ne 0 -and $identity -cnotmatch '^[0-9a-f]{8}:[0-9a-f]{16}$') {
            throw "Release transaction journal has an invalid promoted identity at index $index."
        }
        $promotedIdentities[$index] = $identity
    }

    return [pscustomobject]@{
        Phase = [string]$state.phase
        HadPriorSet = [bool]$state.hadPriorSet
        ExpectedArchiveHashes = $expectedArchiveHashes
        ExpectedFinalBlobHashes = $expectedFinalBlobHashes
        BackupBlobHashes = $backupBlobHashes
        Progress = $progress
        PromotedIdentities = $promotedIdentities
        SerializableState = [ordered]@{
            schemaVersion = 4
            phase = [string]$state.phase
            hadPriorSet = [bool]$state.hadPriorSet
            artifactNames = @($journalNames)
            expectedArchiveHashes = $expectedArchiveHashes
            expectedFinalBlobHashes = $expectedFinalBlobHashes
            backupBlobHashes = $backupBlobHashes
            progress = $progress
            promotedIdentities = @($promotedIdentities)
        }
    }
}

function Open-ExistingReleaseTransaction {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$OutputRoot
    )

    $fullPath = [IO.Path]::GetFullPath($Path)
    $fullRoot = [IO.Path]::TrimEndingDirectorySeparator([IO.Path]::GetFullPath($OutputRoot))
    $leaf = [IO.Path]::GetFileName($fullPath)
    if (-not $fullPath.StartsWith(
            $fullRoot + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase) `
            -or $leaf -cnotmatch '^release-publish-(transaction|committed)-[0-9a-f]{32}$') {
        throw "A stale release publication transaction requires manual recovery: $fullPath"
    }
    $prefix = if ($leaf.StartsWith('release-publish-committed-', [StringComparison]::Ordinal)) {
        'release-publish-committed-'
    }
    else {
        'release-publish-transaction-'
    }
    return [pscustomobject]@{
        Path = $fullPath
        Root = $fullRoot
        LeafPrefix = $prefix
        Lease = [DSRRandomizer.Packaging.SafeDirectoryLease]::Acquire($fullPath)
    }
}

function Remove-ExactReleaseArtifacts {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string[]]$Names,
        [Parameter(Mandatory = $false)][scriptblock]$OperationHook
    )

    for ($index = $Names.Count - 1; $index -ge 0; $index--) {
        $name = $Names[$index]
        $path = Join-Path $Root $name
        if (Test-Path -LiteralPath $path) {
            Invoke-PromotionHook `
                -OperationHook $OperationHook `
                -Phase 'RollbackRemove' `
                -Index $index `
                -Name $name
            $lease = [DSRRandomizer.Packaging.ReleaseArtifactLease]::Acquire(
                $path,
                'release artifact selected for exact removal')
            $lease.Dispose()
            [IO.File]::Delete($path)
        }
    }
}

function Open-VerifiedBackupLeases {
    param(
        [Parameter(Mandatory = $true)][object]$Transaction,
        [Parameter(Mandatory = $true)][string[]]$Names,
        [Parameter(Mandatory = $true)][Collections.IDictionary]$BackupBlobHashes
    )

    $backupNames = @(for ($index = 0; $index -lt $Names.Count; $index++) {
            "previous-$index"
        })
    $leases = Open-ReleaseArtifactSetLeases `
        -Root $Transaction.Path `
        -Names $backupNames `
        -Description 'prior release artifact backup'
    try {
        $actualBlobHashes = [ordered]@{}
        for ($index = 0; $index -lt $Names.Count; $index++) {
            $actualBlobHashes[$Names[$index]] = $leases[$index].ComputeSha256()
        }
        Assert-BlobHashesEqual `
            -Names $Names `
            -Actual $actualBlobHashes `
            -Expected $BackupBlobHashes `
            -Description 'Prior release backup'
        $archiveHashes = [ordered]@{
            $Names[0] = $BackupBlobHashes[$Names[0]]
            $Names[2] = $BackupBlobHashes[$Names[2]]
        }
        $sidecarBlobHashes = Assert-LeasedArtifactSet `
            -Names $Names `
            -Leases $leases `
            -ExpectedArchiveHashes $archiveHashes `
            -Description 'Prior release backup'
        Assert-BlobHashesEqual `
            -Names $Names `
            -Actual $sidecarBlobHashes `
            -Expected $BackupBlobHashes `
            -Description 'Prior release backup'
        return ,$leases
    }
    catch {
        Close-ReleaseArtifactLeases -Leases $leases
        throw
    }
}

function Assert-PreparedCurrentArtifactOwnership {
    param(
        [Parameter(Mandatory = $true)][string]$OutputRoot,
        [Parameter(Mandatory = $true)][string[]]$Names,
        [Parameter(Mandatory = $true)][object]$State
    )

    $current = [Collections.Generic.List[object]]::new()
    try {
        for ($index = 0; $index -lt $Names.Count; $index++) {
            $path = Join-Path $OutputRoot $Names[$index]
            if (-not (Test-Path -LiteralPath $path)) {
                continue
            }
            $lease = [DSRRandomizer.Packaging.ReleaseArtifactLease]::Acquire(
                $path,
                'current artifact inspected for Prepared recovery')
            $current.Add([pscustomobject]@{
                    Index = $index
                    Lease = $lease
                    Hash = $lease.ComputeSha256()
                })
        }

        foreach ($item in $current) {
            $name = $Names[$item.Index]
            $matchesNew = $item.Hash -ceq $State.ExpectedFinalBlobHashes[$name]
            $matchesPrior = $State.HadPriorSet `
                -and $item.Hash -ceq $State.BackupBlobHashes[$name]
            $matchesPromotedIdentity = `
                -not [string]::IsNullOrEmpty($State.PromotedIdentities[$item.Index]) `
                -and $item.Lease.Identity -ceq $State.PromotedIdentities[$item.Index]
            if (-not $matchesNew -and -not $matchesPrior -and -not $matchesPromotedIdentity) {
                throw "Current release artifact '$name' does not belong to the Prepared transaction."
            }
        }

        if ($current.Count -eq $Names.Count) {
            $allNew = $true
            $allPrior = $State.HadPriorSet
            foreach ($item in $current) {
                $name = $Names[$item.Index]
                $allNew = $allNew `
                    -and $item.Hash -ceq $State.ExpectedFinalBlobHashes[$name]
                $allPrior = $allPrior `
                    -and $item.Hash -ceq $State.BackupBlobHashes[$name]
            }
            if ($allNew) {
                $orderedLeases = [Collections.Generic.List[object]]::new()
                foreach ($item in $current | Sort-Object Index) {
                    $orderedLeases.Add($item.Lease)
                }
                $hashes = Assert-LeasedArtifactSet `
                    -Names $Names `
                    -Leases $orderedLeases `
                    -ExpectedArchiveHashes $State.ExpectedArchiveHashes `
                    -Description 'Prepared current new release'
                Assert-BlobHashesEqual `
                    -Names $Names `
                    -Actual $hashes `
                    -Expected $State.ExpectedFinalBlobHashes `
                    -Description 'Prepared current new release'
            }
            elseif ($allPrior) {
                $orderedLeases = [Collections.Generic.List[object]]::new()
                foreach ($item in $current | Sort-Object Index) {
                    $orderedLeases.Add($item.Lease)
                }
                $priorArchiveHashes = [ordered]@{
                    $Names[0] = $State.BackupBlobHashes[$Names[0]]
                    $Names[2] = $State.BackupBlobHashes[$Names[2]]
                }
                $hashes = Assert-LeasedArtifactSet `
                    -Names $Names `
                    -Leases $orderedLeases `
                    -ExpectedArchiveHashes $priorArchiveHashes `
                    -Description 'Prepared current prior release'
                Assert-BlobHashesEqual `
                    -Names $Names `
                    -Actual $hashes `
                    -Expected $State.BackupBlobHashes `
                    -Description 'Prepared current prior release'
            }
        }
    }
    finally {
        for ($index = $current.Count - 1; $index -ge 0; $index--) {
            $current[$index].Lease.Dispose()
        }
        $current.Clear()
    }
}

function Restore-PreparedReleaseTransaction {
    param(
        [Parameter(Mandatory = $true)][object]$Transaction,
        [Parameter(Mandatory = $true)][object]$State,
        [Parameter(Mandatory = $true)][string]$OutputRoot,
        [Parameter(Mandatory = $true)][string[]]$Names,
        [Parameter(Mandatory = $false)][scriptblock]$OperationHook
    )

    $backupLeases = [Collections.Generic.List[object]]::new()
    $restoredLeases = [Collections.Generic.List[object]]::new()
    try {
        Invoke-PromotionHook `
            -OperationHook $OperationHook `
            -Phase 'BeforePreparedRecovery' `
            -Index -1 `
            -Name ''
        if ($State.HadPriorSet) {
            $backupLeases = Open-VerifiedBackupLeases `
                -Transaction $Transaction `
                -Names $Names `
                -BackupBlobHashes $State.BackupBlobHashes
        }
        Assert-PreparedCurrentArtifactOwnership `
            -OutputRoot $OutputRoot `
            -Names $Names `
            -State $State
        Remove-ExactReleaseArtifacts `
            -Root $OutputRoot `
            -Names $Names `
            -OperationHook $OperationHook
        if ($State.HadPriorSet) {
            for ($index = 0; $index -lt $Names.Count; $index++) {
                Invoke-PromotionHook `
                    -OperationHook $OperationHook `
                    -Phase 'RollbackRestore' `
                    -Index $index `
                    -Name $Names[$index]
                $backupLeases[$index].CopyToNewAndFlush(
                    (Join-Path $OutputRoot $Names[$index]))
            }
            $restoredLeases = Open-ReleaseArtifactSetLeases `
                -Root $OutputRoot `
                -Names $Names `
                -Description 'restored prior release artifact'
            $archiveHashes = [ordered]@{
                $Names[0] = $State.BackupBlobHashes[$Names[0]]
                $Names[2] = $State.BackupBlobHashes[$Names[2]]
            }
            $restoredHashes = Assert-LeasedArtifactSet `
                -Names $Names `
                -Leases $restoredLeases `
                -ExpectedArchiveHashes $archiveHashes `
                -Description 'Restored prior release'
            Assert-BlobHashesEqual `
                -Names $Names `
                -Actual $restoredHashes `
                -Expected $State.BackupBlobHashes `
                -Description 'Restored prior release'
        }
    }
    finally {
        Close-ReleaseArtifactLeases -Leases $restoredLeases
        Close-ReleaseArtifactLeases -Leases $backupLeases
    }
    Remove-SafeReleaseDirectory -Directory $Transaction
}

function Convert-ToCommittedReleaseTransaction {
    param([Parameter(Mandatory = $true)][object]$Transaction)

    # The durable journal phase is the commit marker. Keeping the original,
    # leased transaction identity avoids a directory-rename gap at commit.
    return $Transaction
}

function Remove-CommittedReleaseTransaction {
    param(
        [Parameter(Mandatory = $true)][object]$Transaction,
        [Parameter(Mandatory = $true)][string[]]$Names,
        [Parameter(Mandatory = $true)][Collections.IDictionary]$State,
        [Parameter(Mandatory = $false)][scriptblock]$OperationHook
    )

    for ($index = 0; $index -lt $Names.Count; $index++) {
        $backupPath = Join-Path $Transaction.Path "previous-$index"
        if (Test-Path -LiteralPath $backupPath) {
            Invoke-PromotionHook `
                -OperationHook $OperationHook `
                -Phase 'BeforeCommittedBackupCleanup' `
                -Index $index `
                -Name $Names[$index]
            Assert-RegularArtifactFile `
                -Path $backupPath `
                -Description 'Committed prior release backup'
            [IO.File]::Delete($backupPath)
        }
    }
    Invoke-PromotionHook `
        -OperationHook $OperationHook `
        -Phase 'BeforeCommittedJournalCleanup' `
        -Index -1 `
        -Name ''
    Invoke-PromotionHook `
        -OperationHook $OperationHook `
        -Phase 'BeforeCommittedDirectoryCleanup' `
        -Index -1 `
        -Name ''
    try {
        Remove-SafeReleaseDirectory -Directory $Transaction
    }
    catch {
        $cleanupFailure = $_.Exception
        if (Test-Path -LiteralPath $Transaction.Path -PathType Container) {
            if ($null -eq $Transaction.Lease) {
                $reopened = Open-ExistingReleaseTransaction `
                    -Path $Transaction.Path `
                    -OutputRoot $Transaction.Root
                $Transaction.Lease = $reopened.Lease
            }
            $statePath = Join-Path $Transaction.Path 'transaction-state.json'
            if (-not (Test-Path -LiteralPath $statePath -PathType Leaf)) {
                try {
                    Set-ReleaseTransactionState `
                        -TransactionRoot $Transaction.Path `
                        -State $State | Out-Null
                }
                catch {
                    throw [IO.IOException]::new(
                        "Committed transaction cleanup failed and its durable Committed journal could not be recreated. Cleanup failure: $($cleanupFailure.Message) Journal failure: $($_.Exception.Message)",
                        $cleanupFailure)
                }
            }
        }
        throw $cleanupFailure
    }
}

function Repair-ReleasePublicationState {
    param(
        [Parameter(Mandatory = $true)][object]$OutputDirectory,
        [Parameter(Mandatory = $true)][string[]]$Names,
        [Parameter(Mandatory = $false)][scriptblock]$OperationHook
    )

    $pending = @(Get-ChildItem -LiteralPath $OutputDirectory.Path -Directory -Force | Where-Object {
            $_.Name.StartsWith('release-publish-transaction-', [StringComparison]::Ordinal) `
                -or $_.Name.StartsWith('release-publish-committed-', [StringComparison]::Ordinal)
        })
    if ($pending.Count -eq 0) {
        return
    }
    if ($pending.Count -ne 1) {
        throw "Multiple stale release publication transactions require manual recovery: $($pending.FullName -join ', ')"
    }

    $transaction = $null
    $finalLeases = [Collections.Generic.List[object]]::new()
    try {
        $transaction = Open-ExistingReleaseTransaction `
            -Path $pending[0].FullName `
            -OutputRoot $OutputDirectory.Path
        $state = Read-ReleaseTransactionState `
            -TransactionRoot $transaction.Path `
            -Names $Names
        if ($state.Phase -ceq 'Prepared') {
            Restore-PreparedReleaseTransaction `
                -Transaction $transaction `
                -State $state `
                -OutputRoot $OutputDirectory.Path `
                -Names $Names `
                -OperationHook $OperationHook
            $transaction = $null
        }
        else {
            $finalLeases = Open-ReleaseArtifactSetLeases `
                -Root $OutputDirectory.Path `
                -Names $Names `
                -Description 'committed published release artifact'
            $finalHashes = Assert-LeasedArtifactSet `
                -Names $Names `
                -Leases $finalLeases `
                -ExpectedArchiveHashes $state.ExpectedArchiveHashes `
                -Description 'Committed published release'
            Assert-BlobHashesEqual `
                -Names $Names `
                -Actual $finalHashes `
                -Expected $state.ExpectedFinalBlobHashes `
                -Description 'Committed published release'
            $transaction = Convert-ToCommittedReleaseTransaction -Transaction $transaction
            Remove-CommittedReleaseTransaction `
                -Transaction $transaction `
                -Names $Names `
                -State $state.SerializableState `
                -OperationHook $OperationHook
            $transaction = $null
        }
        Invoke-PromotionHook `
            -OperationHook $OperationHook `
            -Phase 'AfterRecovery' `
            -Index -1 `
            -Name ''
    }
    catch {
        if ($null -ne $transaction -and $null -ne $transaction.Lease) {
            $transaction.Lease.Dispose()
            $transaction.Lease = $null
        }
        throw
    }
    finally {
        Close-ReleaseArtifactLeases -Leases $finalLeases
    }
}

function Publish-ReleaseArtifactSet {
    param(
        [Parameter(Mandatory = $true)][string]$StagingRoot,
        [Parameter(Mandatory = $true)][string]$OutputRoot,
        [Parameter(Mandatory = $true)][ValidateCount(4, 4)][string[]]$ArtifactNames,
        [Parameter(Mandatory = $true)][Collections.IDictionary]$ExpectedArchiveHashes,
        [Parameter(Mandatory = $false)][scriptblock]$OperationHook
    )

    $names = [Collections.Generic.List[string]]::new()
    $nameSet = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($name in $ArtifactNames) {
        if ([string]::IsNullOrWhiteSpace($name) `
                -or [IO.Path]::IsPathRooted($name) `
                -or [IO.Path]::GetFileName($name) -cne $name `
                -or $name.Contains([IO.Path]::DirectorySeparatorChar) `
                -or $name.Contains([IO.Path]::AltDirectorySeparatorChar)) {
            throw "Release artifact name is not a plain file name: $name"
        }
        if (-not $nameSet.Add($name)) {
            throw "Release artifact names are not unique: $name"
        }
        $names.Add($name)
    }
    $validatedExpectedHashes = Get-ValidatedExpectedArchiveHashes `
        -Names @($names) `
        -ExpectedArchiveHashes $ExpectedArchiveHashes

    $stagingDirectory = Open-SafeReleaseRoot -Path $StagingRoot
    $outputDirectory = $null
    $transaction = $null
    $stagedLeases = [Collections.Generic.List[object]]::new()
    $priorLeases = [Collections.Generic.List[object]]::new()
    $backupLeases = [Collections.Generic.List[object]]::new()
    $finalLeases = [Collections.Generic.List[object]]::new()
    $publicationLock = $null
    $committed = $false
    try {
        $outputDirectory = Open-SafeReleaseRoot -Path $OutputRoot
        if ($stagingDirectory.Path.Equals(
                $outputDirectory.Path,
                [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Release staging and output roots must be different directories.'
        }

        $publicationLock = [DSRRandomizer.Packaging.ReleasePublicationLock]::Acquire(
            $outputDirectory.Path)
        Invoke-PromotionHook `
            -OperationHook $OperationHook `
            -Phase 'AfterPublicationLockAcquired' `
            -Index -1 `
            -Name ''

        Repair-ReleasePublicationState `
            -OutputDirectory $outputDirectory `
            -Names @($names) `
            -OperationHook $OperationHook

        $stagedLeases = Open-ReleaseArtifactSetLeases `
            -Root $stagingDirectory.Path `
            -Names @($names) `
            -Description 'staged release artifact'
        $stagedBlobHashes = Assert-LeasedArtifactSet `
            -Names @($names) `
            -Leases $stagedLeases `
            -ExpectedArchiveHashes $validatedExpectedHashes `
            -Description 'Staged release'

        $priorNames = [Collections.Generic.List[string]]::new()
        foreach ($name in $names) {
            if (Test-Path -LiteralPath (Join-Path $outputDirectory.Path $name)) {
                $priorNames.Add($name)
            }
        }
        if ($priorNames.Count -ne 0 -and $priorNames.Count -ne $names.Count) {
            throw "The output contains a partial prior release artifact set ($($priorNames.Count) of $($names.Count)); publication is fail-closed."
        }
        $hasPriorSet = $priorNames.Count -eq $names.Count
        $priorBlobHashes = [ordered]@{}
        if ($hasPriorSet) {
            $priorLeases = Open-ReleaseArtifactSetLeases `
                -Root $outputDirectory.Path `
                -Names @($names) `
                -Description 'prior release artifact'
            $priorArchiveHashes = Get-ArchiveHashesFromLeases `
                -Names @($names) `
                -Leases $priorLeases
            $priorBlobHashes = Assert-LeasedArtifactSet `
                -Names @($names) `
                -Leases $priorLeases `
                -ExpectedArchiveHashes $priorArchiveHashes `
                -Description 'Prior release'
        }

        $transaction = New-SafeReleaseDirectory `
            -TrustedRoot $outputDirectory.Path `
            -LeafPrefix 'release-publish-transaction-'
        try {
            Invoke-PromotionHook `
                -OperationHook $OperationHook `
                -Phase 'BeforeInitialJournal' `
                -Index -1 `
                -Name ''

            if ($hasPriorSet) {
                for ($index = 0; $index -lt $names.Count; $index++) {
                    Invoke-PromotionHook `
                        -OperationHook $OperationHook `
                        -Phase 'BeforeBackup' `
                        -Index $index `
                        -Name $names[$index]
                    $priorLeases[$index].CopyToNewAndFlush(
                        (Join-Path $transaction.Path "previous-$index"))
                }
                $backupLeases = Open-VerifiedBackupLeases `
                    -Transaction $transaction `
                    -Names @($names) `
                    -BackupBlobHashes $priorBlobHashes
            }

            $state = [ordered]@{
                schemaVersion = 4
                phase = 'Prepared'
                hadPriorSet = $hasPriorSet
                artifactNames = @($names)
                expectedArchiveHashes = $validatedExpectedHashes
                expectedFinalBlobHashes = $stagedBlobHashes
                backupBlobHashes = $priorBlobHashes
                progress = 'BackupsVerified'
                promotedIdentities = @('', '', '', '')
            }
            Set-ReleaseTransactionState `
                -TransactionRoot $transaction.Path `
                -State $state `
                -OperationHook $OperationHook | Out-Null

            Close-ReleaseArtifactLeases -Leases $priorLeases
            for ($index = 0; $index -lt $names.Count; $index++) {
                $outputPath = Join-Path $outputDirectory.Path $names[$index]
                if (Test-Path -LiteralPath $outputPath) {
                    $currentLease = [DSRRandomizer.Packaging.ReleaseArtifactLease]::Acquire(
                        $outputPath,
                        'prior release artifact selected for replacement')
                    try {
                        if (-not $hasPriorSet `
                                -or $currentLease.ComputeSha256() -cne $priorBlobHashes[$names[$index]]) {
                            throw "Prior release artifact changed before replacement: $($names[$index])"
                        }
                    }
                    finally {
                        $currentLease.Dispose()
                    }
                    [IO.File]::Delete($outputPath)
                }
            }
            $state.progress = 'PriorRemoved'
            Set-ReleaseTransactionState `
                -TransactionRoot $transaction.Path `
                -State $state `
                -OperationHook $OperationHook | Out-Null

            for ($index = 0; $index -lt $names.Count; $index++) {
                Invoke-PromotionHook `
                    -OperationHook $OperationHook `
                    -Phase 'BeforePublishCopy' `
                    -Index $index `
                    -Name $names[$index]
                $outputPath = Join-Path $outputDirectory.Path $names[$index]
                if (Test-Path -LiteralPath $outputPath) {
                    throw "The exact publication destination unexpectedly exists: $outputPath"
                }
                $stagedLeases[$index].CopyToNewAndFlush($outputPath)
                $promotedLease = [DSRRandomizer.Packaging.ReleaseArtifactLease]::Acquire(
                    $outputPath,
                    'newly promoted release artifact')
                try {
                    if ($promotedLease.ComputeSha256() -cne $stagedBlobHashes[$names[$index]]) {
                        throw "Newly promoted release artifact hash mismatch: $($names[$index])"
                    }
                    $state.promotedIdentities[$index] = $promotedLease.Identity
                }
                finally {
                    $promotedLease.Dispose()
                }
                $state.progress = 'Publishing'
                Set-ReleaseTransactionState `
                    -TransactionRoot $transaction.Path `
                    -State $state `
                    -OperationHook $OperationHook | Out-Null
            }
            $state.progress = 'FinalsWritten'
            Set-ReleaseTransactionState `
                -TransactionRoot $transaction.Path `
                -State $state `
                -OperationHook $OperationHook | Out-Null

            Invoke-PromotionHook `
                -OperationHook $OperationHook `
                -Phase 'BeforeFinalVerification' `
                -Index -1 `
                -Name ''
            $finalLeases = Open-ReleaseArtifactSetLeases `
                -Root $outputDirectory.Path `
                -Names @($names) `
                -Description 'published release artifact'
            $finalBlobHashes = Assert-LeasedArtifactSet `
                -Names @($names) `
                -Leases $finalLeases `
                -ExpectedArchiveHashes $validatedExpectedHashes `
                -Description 'Published release'
            Assert-BlobHashesEqual `
                -Names @($names) `
                -Actual $finalBlobHashes `
                -Expected $stagedBlobHashes `
                -Description 'Published release'

            $preparedFallback = [ordered]@{
                schemaVersion = 4
                phase = 'Prepared'
                hadPriorSet = $state.hadPriorSet
                artifactNames = @($state.artifactNames)
                expectedArchiveHashes = $state.expectedArchiveHashes
                expectedFinalBlobHashes = $state.expectedFinalBlobHashes
                backupBlobHashes = $state.backupBlobHashes
                progress = 'FinalsWritten'
                promotedIdentities = @($state.promotedIdentities)
            }
            $state.phase = 'Committed'
            $state.progress = 'FinalVerified'
            Set-ReleaseTransactionState `
                -TransactionRoot $transaction.Path `
                -State $state `
                -OperationHook $OperationHook `
                -FallbackState $preparedFallback | Out-Null
            $committed = $true
            $transaction = Convert-ToCommittedReleaseTransaction -Transaction $transaction

            Close-ReleaseArtifactLeases -Leases $backupLeases
            Remove-CommittedReleaseTransaction `
                -Transaction $transaction `
                -Names @($names) `
                -State $state `
                -OperationHook $OperationHook
            $transaction = $null
        }
        catch {
            $publicationFailure = $_.Exception
            if ($committed) {
                if ($null -ne $transaction -and $null -ne $transaction.Lease) {
                    $transaction.Lease.Dispose()
                    $transaction.Lease = $null
                }
                $retainedPath = if ($null -eq $transaction) {
                    '<committed cleanup path unavailable>'
                }
                else {
                    $transaction.Path
                }
                $transaction = $null
                throw [IO.IOException]::new(
                    "Release artifact publication committed the complete verified new set, but committed transaction cleanup is pending at '$retainedPath': $($publicationFailure.Message)",
                    $publicationFailure)
            }

            Close-ReleaseArtifactLeases -Leases $finalLeases
            Close-ReleaseArtifactLeases -Leases $priorLeases
            Close-ReleaseArtifactLeases -Leases $backupLeases
            $recoveryFailure = $null
            try {
                if (Test-Path -LiteralPath (Join-Path $transaction.Path 'transaction-state.json')) {
                    $preparedState = Read-ReleaseTransactionState `
                        -TransactionRoot $transaction.Path `
                        -Names @($names)
                    if ($preparedState.Phase -cne 'Prepared') {
                        throw 'An uncommitted publication transaction did not retain Prepared state.'
                    }
                    Restore-PreparedReleaseTransaction `
                        -Transaction $transaction `
                        -State $preparedState `
                        -OutputRoot $outputDirectory.Path `
                        -Names @($names) `
                        -OperationHook $OperationHook
                    $transaction = $null
                }
                else {
                    Remove-SafeReleaseDirectory -Directory $transaction
                    $transaction = $null
                }
            }
            catch {
                $recoveryFailure = $_.Exception
            }

            if ($null -ne $recoveryFailure) {
                $retainedPath = if ($null -eq $transaction) {
                    '<recovery path unavailable>'
                }
                else {
                    $transaction.Path
                }
                if ($null -ne $transaction -and $null -ne $transaction.Lease) {
                    $transaction.Lease.Dispose()
                    $transaction.Lease = $null
                }
                $transaction = $null
                throw [IO.IOException]::new(
                    "Release artifact publication failed and Prepared rollback was incomplete. Transaction retained at '$retainedPath'. Publication failure: $($publicationFailure.Message) Recovery failure: $($recoveryFailure.Message)",
                    $publicationFailure)
            }
            throw [IO.IOException]::new(
                "Release artifact publication failed; the prior complete set was preserved or restored: $($publicationFailure.Message)",
                $publicationFailure)
        }
    }
    finally {
        Close-ReleaseArtifactLeases -Leases $finalLeases
        Close-ReleaseArtifactLeases -Leases $backupLeases
        Close-ReleaseArtifactLeases -Leases $priorLeases
        Close-ReleaseArtifactLeases -Leases $stagedLeases
        if ($null -ne $transaction -and $null -ne $transaction.Lease) {
            $transaction.Lease.Dispose()
        }
        if ($null -ne $publicationLock) {
            $publicationLock.Dispose()
        }
        if ($null -ne $outputDirectory) {
            $outputDirectory.Lease.Dispose()
        }
        $stagingDirectory.Lease.Dispose()
    }
}

Export-ModuleMember -Function Publish-ReleaseArtifactSet
