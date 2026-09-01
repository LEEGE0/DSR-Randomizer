Set-StrictMode -Version Latest

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

if ($null -eq ('DSRRandomizer.Packaging.ReleasePrivacyScanner' -as [type])) {
    Add-Type -TypeDefinition @'
using System;

namespace DSRRandomizer.Packaging
{
    public static class ReleasePrivacyScanner
    {
        public static bool Contains(byte[] bytes, byte[] pattern)
        {
            if (bytes == null)
            {
                throw new ArgumentNullException(nameof(bytes));
            }
            if (pattern == null || pattern.Length == 0)
            {
                throw new ArgumentException("A non-empty privacy pattern is required.", nameof(pattern));
            }
            return bytes.AsSpan().IndexOf(pattern) >= 0;
        }
    }
}
'@
}

function New-ReleasePrivacySentinelSet {
    param(
        [Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$LocalUserName,
        [Parameter(Mandatory = $true)][ValidatePattern('^[0-9]{1,20}$')][string]$ShortAccountId
    )

    $windowsPath = "C:\Users\$LocalUserName"
    @(
        [pscustomobject]@{
            Name = 'local Windows profile path'
            Value = $windowsPath
        },
        [pscustomobject]@{
            Name = 'JSON-escaped local Windows profile path'
            Value = $windowsPath.Replace('\', '\\')
        },
        [pscustomobject]@{
            Name = 'forward-slash local Windows profile path'
            Value = "C:/Users/$LocalUserName"
        },
        [pscustomobject]@{
            Name = 'local profile file URI'
            Value = "file:///C:/Users/$LocalUserName"
        },
        [pscustomobject]@{
            Name = 'reviewed short account identifier'
            Value = $ShortAccountId
        }
    )
}

function Get-ReviewedReleasePrivacySentinels {
    $reviewedUserName = 'Us' + 'er'
    $reviewedShortAccountId = '146' + '808' + '034'
    New-ReleasePrivacySentinelSet `
        -LocalUserName $reviewedUserName `
        -ShortAccountId $reviewedShortAccountId
}

function Assert-ReleasePrivacyBuffer {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][byte[]]$Bytes,
        [Parameter(Mandatory = $true)][object[]]$Sentinels
    )

    $encodings = @(
        [pscustomobject]@{
            Name = 'UTF-8'
            Encoding = [Text.UTF8Encoding]::new($false)
        },
        [pscustomobject]@{
            Name = 'UTF-16LE'
            Encoding = [Text.UnicodeEncoding]::new($false, $false)
        },
        [pscustomobject]@{
            Name = 'UTF-16BE'
            Encoding = [Text.UnicodeEncoding]::new($true, $false)
        }
    )
    foreach ($sentinel in $Sentinels) {
        $name = [string]$sentinel.Name
        $value = [string]$sentinel.Value
        if ([string]::IsNullOrEmpty($name) -or [string]::IsNullOrEmpty($value)) {
            throw 'Release privacy sentinels require non-empty names and values.'
        }
        foreach ($encoded in $encodings) {
            $pattern = $encoded.Encoding.GetBytes($value)
            if ([DSRRandomizer.Packaging.ReleasePrivacyScanner]::Contains($Bytes, $pattern)) {
                throw "Release privacy sentinel '$name' was found in '$Path' as $($encoded.Name)."
            }
        }
    }
}

function Assert-ReleasePrivacyBytes {
    param(
        [Parameter(Mandatory = $true)][Collections.IDictionary]$Files,
        [Parameter(Mandatory = $false)][object[]]$Sentinels = @(
            Get-ReviewedReleasePrivacySentinels)
    )

    foreach ($entry in $Files.GetEnumerator()) {
        Assert-ReleasePrivacyBuffer `
            -Path ([string]$entry.Key) `
            -Bytes ([byte[]]$entry.Value) `
            -Sentinels $Sentinels
    }
}

function Assert-ReleaseArchivePrivacy {
    param(
        [Parameter(Mandatory = $true)][string]$ArchivePath,
        [Parameter(Mandatory = $false)][object[]]$Sentinels = @(
            Get-ReviewedReleasePrivacySentinels)
    )

    $resolvedArchive = [IO.Path]::GetFullPath($ArchivePath)
    if (-not (Test-Path -LiteralPath $resolvedArchive -PathType Leaf)) {
        throw "Release privacy archive is missing: $resolvedArchive"
    }

    $archive = [IO.Compression.ZipFile]::OpenRead($resolvedArchive)
    try {
        foreach ($entry in $archive.Entries) {
            if ($entry.FullName.EndsWith('/', [StringComparison]::Ordinal)) {
                continue
            }
            $stream = $entry.Open()
            try {
                $memory = [IO.MemoryStream]::new()
                try {
                    $stream.CopyTo($memory)
                    Assert-ReleasePrivacyBuffer `
                        -Path $entry.FullName `
                        -Bytes $memory.ToArray() `
                        -Sentinels $Sentinels
                }
                finally {
                    $memory.Dispose()
                }
            }
            finally {
                $stream.Dispose()
            }
        }
    }
    finally {
        $archive.Dispose()
    }
}

Export-ModuleMember -Function `
    New-ReleasePrivacySentinelSet, `
    Get-ReviewedReleasePrivacySentinels, `
    Assert-ReleasePrivacyBytes, `
    Assert-ReleaseArchivePrivacy
