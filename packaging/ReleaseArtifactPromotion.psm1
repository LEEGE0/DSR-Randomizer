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
        [Parameter(Mandatory = $true)][Collections.IDictionary]$State
    )

    $statePath = Join-Path $TransactionRoot 'transaction-state.json'
    $temporaryStatePath = Join-Path $TransactionRoot 'transaction-state.next'
    $json = $State | ConvertTo-Json -Compress -Depth 4
    [IO.File]::WriteAllText(
        $temporaryStatePath,
        $json,
        [Text.UTF8Encoding]::new($false))
    [IO.File]::Move($temporaryStatePath, $statePath, $true)
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
    $finalLeases = [Collections.Generic.List[object]]::new()
    try {
        $outputDirectory = Open-SafeReleaseRoot -Path $OutputRoot
        if ($stagingDirectory.Path.Equals(
                $outputDirectory.Path,
                [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Release staging and output roots must be different directories.'
        }

        $staleTransactions = @(Get-ChildItem `
            -LiteralPath $outputDirectory.Path `
            -Directory `
            -Force `
            -Filter 'release-publish-transaction-*')
        if ($staleTransactions.Count -ne 0) {
            throw "A stale release publication transaction requires manual recovery: $($staleTransactions.FullName -join ', ')"
        }

        $priorNames = [Collections.Generic.List[string]]::new()
        foreach ($name in $names) {
            $outputPath = Join-Path $outputDirectory.Path $name
            if (Test-Path -LiteralPath $outputPath) {
                Assert-RegularArtifactFile `
                    -Path $outputPath `
                    -Description 'Prior release artifact'
                $priorNames.Add($name)
            }
        }
        if ($priorNames.Count -ne 0 -and $priorNames.Count -ne $names.Count) {
            throw "The output contains a partial prior release artifact set ($($priorNames.Count) of $($names.Count)); publication is fail-closed."
        }
        $hasPriorSet = $priorNames.Count -eq $names.Count

        $state = [ordered]@{
            schemaVersion = 2
            phase = 'created'
            hadPriorSet = $hasPriorSet
            artifactNames = @($names)
            stagedIdentities = @()
            backedUp = @()
            promoted = @()
        }
        $backedUp = [Collections.Generic.List[string]]::new()
        $published = [Collections.Generic.List[string]]::new()
        $transaction = New-SafeReleaseDirectory `
            -TrustedRoot $outputDirectory.Path `
            -LeafPrefix 'release-publish-transaction-'
        try {
            Invoke-PromotionHook `
                -OperationHook $OperationHook `
                -Phase 'BeforeInitialJournal' `
                -Index -1 `
                -Name ''
            Set-ReleaseTransactionState `
                -TransactionRoot $transaction.Path `
                -State $state

            foreach ($name in $names) {
                $stagedLeases.Add(
                    [DSRRandomizer.Packaging.ReleaseArtifactLease]::Acquire(
                        (Join-Path $stagingDirectory.Path $name),
                        'staged release artifact'))
            }
            $state.stagedIdentities = @($stagedLeases | ForEach-Object { $_.Identity })
            $stagedBlobHashes = Assert-LeasedArtifactSet `
                -Names @($names) `
                -Leases $stagedLeases `
                -ExpectedArchiveHashes $validatedExpectedHashes `
                -Description 'Staged release'
            $state.phase = 'staged-verified'
            Set-ReleaseTransactionState `
                -TransactionRoot $transaction.Path `
                -State $state

            $state.phase = 'backing-up'
            Set-ReleaseTransactionState `
                -TransactionRoot $transaction.Path `
                -State $state
            if ($hasPriorSet) {
                for ($index = 0; $index -lt $names.Count; $index++) {
                    $name = $names[$index]
                    Invoke-PromotionHook `
                        -OperationHook $OperationHook `
                        -Phase 'BeforeBackup' `
                        -Index $index `
                        -Name $name
                    [IO.File]::Move(
                        (Join-Path $outputDirectory.Path $name),
                        (Join-Path $transaction.Path ("previous-$index")),
                        $false)
                    $backedUp.Add($name)
                    $state.backedUp = @($backedUp)
                    Set-ReleaseTransactionState `
                        -TransactionRoot $transaction.Path `
                        -State $state
                }
            }

            $state.phase = 'publishing'
            Set-ReleaseTransactionState `
                -TransactionRoot $transaction.Path `
                -State $state
            for ($index = 0; $index -lt $names.Count; $index++) {
                $name = $names[$index]
                Invoke-PromotionHook `
                    -OperationHook $OperationHook `
                    -Phase 'BeforePublishCopy' `
                    -Index $index `
                    -Name $name
                $outputPath = Join-Path $outputDirectory.Path $name
                if (Test-Path -LiteralPath $outputPath) {
                    throw "The exact publication destination unexpectedly exists: $outputPath"
                }
                $published.Add($name)
                $stagedLeases[$index].CopyToNewAndFlush($outputPath)
                $state.promoted = @($published)
                Set-ReleaseTransactionState `
                    -TransactionRoot $transaction.Path `
                    -State $state
            }

            Invoke-PromotionHook `
                -OperationHook $OperationHook `
                -Phase 'BeforeFinalVerification' `
                -Index -1 `
                -Name ''
            foreach ($name in $names) {
                $finalLeases.Add(
                    [DSRRandomizer.Packaging.ReleaseArtifactLease]::Acquire(
                        (Join-Path $outputDirectory.Path $name),
                        'published release artifact'))
            }
            $finalBlobHashes = Assert-LeasedArtifactSet `
                -Names @($names) `
                -Leases $finalLeases `
                -ExpectedArchiveHashes $validatedExpectedHashes `
                -Description 'Published release'
            foreach ($name in $names) {
                if ($finalBlobHashes[$name] -cne $stagedBlobHashes[$name]) {
                    throw "Published release artifact verification failed: $name"
                }
            }

            $state.phase = 'complete'
            Set-ReleaseTransactionState `
                -TransactionRoot $transaction.Path `
                -State $state
            Remove-SafeReleaseDirectory -Directory $transaction
            $transaction = $null
        }
        catch {
            $publicationFailure = $_.Exception
            Close-ReleaseArtifactLeases -Leases $finalLeases
            $rollbackFailures = [Collections.Generic.List[string]]::new()
            $state.phase = 'rolling-back'
            try {
                Set-ReleaseTransactionState `
                    -TransactionRoot $transaction.Path `
                    -State $state
            }
            catch {
                # The in-memory state remains authoritative for cleanup and rollback.
            }

            for ($index = $published.Count - 1; $index -ge 0; $index--) {
                $name = $published[$index]
                try {
                    Invoke-PromotionHook `
                        -OperationHook $OperationHook `
                        -Phase 'RollbackRemove' `
                        -Index $index `
                        -Name $name
                    $outputPath = Join-Path $outputDirectory.Path $name
                    if (Test-Path -LiteralPath $outputPath) {
                        Assert-RegularArtifactFile `
                            -Path $outputPath `
                            -Description 'Newly published release artifact'
                        [IO.File]::Delete($outputPath)
                    }
                }
                catch {
                    $rollbackFailures.Add("Unable to remove newly published '$name': $($_.Exception.Message)")
                }
            }
            for ($index = $backedUp.Count - 1; $index -ge 0; $index--) {
                $name = $backedUp[$index]
                try {
                    Invoke-PromotionHook `
                        -OperationHook $OperationHook `
                        -Phase 'RollbackRestore' `
                        -Index $index `
                        -Name $name
                    $backupPath = Join-Path $transaction.Path ("previous-$index")
                    Assert-RegularArtifactFile `
                        -Path $backupPath `
                        -Description 'Prior release artifact backup'
                    $outputPath = Join-Path $outputDirectory.Path $name
                    if (Test-Path -LiteralPath $outputPath) {
                        throw "Rollback destination unexpectedly exists: $outputPath"
                    }
                    [IO.File]::Move($backupPath, $outputPath, $false)
                }
                catch {
                    $rollbackFailures.Add("Unable to restore prior '$name': $($_.Exception.Message)")
                }
            }

            if ($rollbackFailures.Count -eq 0) {
                try {
                    $state.phase = 'rolled-back'
                    try {
                        Set-ReleaseTransactionState `
                            -TransactionRoot $transaction.Path `
                            -State $state
                    }
                    catch {
                        # Cleanup does not depend on the advisory journal.
                    }
                    Remove-SafeReleaseDirectory -Directory $transaction
                    $transaction = $null
                }
                catch {
                    $rollbackFailures.Add("Unable to remove publication transaction: $($_.Exception.Message)")
                }
            }

            if ($rollbackFailures.Count -ne 0) {
                if ($null -ne $transaction -and $null -ne $transaction.Lease) {
                    $transaction.Lease.Dispose()
                    $transaction.Lease = $null
                }
                $retainedTransactionPath = if ($null -eq $transaction) {
                    '<cleanup failed after transaction identity was released>'
                }
                else {
                    $transaction.Path
                }
                $transaction = $null
                throw [IO.IOException]::new(
                    "Release artifact publication failed and rollback or cleanup was incomplete. " +
                    "Transaction retained at '$retainedTransactionPath'. " +
                    "Publication failure: $($publicationFailure.Message) " +
                    "Recovery failures: $($rollbackFailures -join ' | ')",
                    $publicationFailure)
            }

            throw [IO.IOException]::new(
                "Release artifact publication failed; the prior complete set was preserved or restored: $($publicationFailure.Message)",
                $publicationFailure)
        }
    }
    finally {
        Close-ReleaseArtifactLeases -Leases $finalLeases
        Close-ReleaseArtifactLeases -Leases $stagedLeases
        if ($null -ne $transaction -and $null -ne $transaction.Lease) {
            $transaction.Lease.Dispose()
        }
        if ($null -ne $outputDirectory) {
            $outputDirectory.Lease.Dispose()
        }
        $stagingDirectory.Lease.Dispose()
    }
}

Export-ModuleMember -Function Publish-ReleaseArtifactSet
