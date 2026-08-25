using System.Buffers.Binary;
using System.IO.MemoryMappedFiles;
using System.Security.Cryptography;

namespace DSRRandomizer.Foundation.Safety;

public enum ProfileError
{
    None,
    InvalidPeImage,
    MachineMismatch,
    LengthMismatch,
    FileHashMismatch,
    PeTimestampMismatch,
    ImageSizeMismatch,
    TargetModuleMismatch,
    TargetOutOfRange,
    TargetFingerprintMismatch,
    ModuleMissing
}

public readonly record struct ProfileVerificationResult(
    ProfileError Error,
    string? Detail = null)
{
    internal static ProfileVerificationResult Success { get; } = new(ProfileError.None);
}

public static class ProfileInspector
{
    private const ushort Pe32PlusMagic = 0x20b;
    private const uint ExecutableSection = 0x20000000;

    public static ProfileVerificationResult VerifyImage(
        byte[] image,
        string moduleName,
        ExecutableIdentity expectedIdentity,
        IReadOnlyList<InternalTargetProfile> targets)
    {
        ArgumentNullException.ThrowIfNull(image);
        ArgumentException.ThrowIfNullOrWhiteSpace(moduleName);
        ArgumentNullException.ThrowIfNull(expectedIdentity);
        ArgumentNullException.ThrowIfNull(targets);

        if (image.LongLength != expectedIdentity.Length)
        {
            return new(ProfileError.LengthMismatch);
        }

        var actualHash = Convert.ToHexString(SHA256.HashData(image)).ToLowerInvariant();
        if (!string.Equals(actualHash, expectedIdentity.Sha256, StringComparison.OrdinalIgnoreCase))
        {
            return new(ProfileError.FileHashMismatch);
        }

        if (!TryReadHeaders(image, out var headers))
        {
            return new(ProfileError.InvalidPeImage);
        }

        if (headers.Machine != expectedIdentity.Machine)
        {
            return new(ProfileError.MachineMismatch);
        }
        if (headers.Timestamp != expectedIdentity.PeTimestamp)
        {
            return new(ProfileError.PeTimestampMismatch);
        }
        if (headers.SizeOfImage != expectedIdentity.SizeOfImage)
        {
            return new(ProfileError.ImageSizeMismatch);
        }

        foreach (var target in targets)
        {
            if (!string.Equals(target.Module, moduleName, StringComparison.OrdinalIgnoreCase))
            {
                return new(ProfileError.TargetModuleMismatch);
            }
            if (!TryMapExecutableWindow(image, headers, target.Rva, target.PatchLength, out var window))
            {
                return new(ProfileError.TargetOutOfRange);
            }
            var fingerprint = Convert.ToHexString(SHA256.HashData(window)).ToLowerInvariant();
            if (!string.Equals(fingerprint, target.FingerprintSha256, StringComparison.OrdinalIgnoreCase))
            {
                return new(ProfileError.TargetFingerprintMismatch);
            }
        }

        return ProfileVerificationResult.Success;
    }

    public static ProfileVerificationResult VerifyFiles(
        string executablePath,
        CompatibilityProfile profile)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(executablePath);
        ArgumentNullException.ThrowIfNull(profile);

        if (!string.Equals(
                Path.GetFileName(executablePath),
                profile.ExecutableModule,
                StringComparison.OrdinalIgnoreCase))
        {
            return new(ProfileError.ModuleMissing);
        }

        try
        {
            var directory = Path.GetDirectoryName(Path.GetFullPath(executablePath));
            if (string.IsNullOrWhiteSpace(directory))
            {
                return new(ProfileError.ModuleMissing);
            }

            var executable = ReadMappedFile(executablePath);
            var result = VerifyImage(
                executable,
                profile.ExecutableModule,
                profile.Executable,
                profile.GameServiceTargets.Where(target =>
                    string.Equals(target.Module, profile.ExecutableModule,
                        StringComparison.OrdinalIgnoreCase)).ToArray());
            if (result.Error != ProfileError.None)
            {
                return result;
            }

            foreach (var module in profile.Modules)
            {
                var modulePath = Path.Combine(directory, module.Name);
                if (!File.Exists(modulePath) ||
                    !string.Equals(Path.GetFileName(modulePath), module.Name,
                        StringComparison.OrdinalIgnoreCase))
                {
                    return new(ProfileError.ModuleMissing);
                }

                var image = ReadMappedFile(modulePath);
                result = VerifyImage(
                    image,
                    module.Name,
                    module.Identity,
                    profile.GameServiceTargets.Where(target =>
                        string.Equals(target.Module, module.Name,
                            StringComparison.OrdinalIgnoreCase)).ToArray());
                if (result.Error != ProfileError.None)
                {
                    return result;
                }
            }

            return ProfileVerificationResult.Success;
        }
        catch (Exception exception) when (
            exception is IOException or UnauthorizedAccessException or NotSupportedException)
        {
            return new(ProfileError.ModuleMissing);
        }
    }

    public static ExecutableIdentity InspectIdentity(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        var image = ReadMappedFile(path);
        if (!TryReadHeaders(image, out var headers))
        {
            throw new BadImageFormatException("The module is not a supported PE32+ image.");
        }
        return new ExecutableIdentity(
            image.LongLength,
            Convert.ToHexString(SHA256.HashData(image)).ToLowerInvariant(),
            headers.Machine,
            headers.Timestamp,
            headers.SizeOfImage);
    }

    private static byte[] ReadMappedFile(string path)
    {
        var length = new FileInfo(path).Length;
        if (length <= 0 || length > int.MaxValue)
        {
            throw new IOException("The module has an unsupported length.");
        }

        using var mapping = MemoryMappedFile.CreateFromFile(
            path,
            FileMode.Open,
            mapName: null,
            capacity: 0,
            MemoryMappedFileAccess.Read);
        using var view = mapping.CreateViewStream(0, length, MemoryMappedFileAccess.Read);
        var bytes = GC.AllocateUninitializedArray<byte>(checked((int)length));
        view.ReadExactly(bytes);
        return bytes;
    }

    private static bool TryReadHeaders(byte[] image, out PeHeaders headers)
    {
        headers = default;
        if (image.Length < 0x40 || image[0] != 'M' || image[1] != 'Z')
        {
            return false;
        }

        var peOffset = BinaryPrimitives.ReadInt32LittleEndian(image.AsSpan(0x3c));
        if (peOffset < 0 || peOffset > image.Length - 24 ||
            !image.AsSpan(peOffset, 4).SequenceEqual("PE\0\0"u8))
        {
            return false;
        }

        var coff = image.AsSpan(peOffset + 4);
        var machine = BinaryPrimitives.ReadUInt16LittleEndian(coff);
        var sectionCount = BinaryPrimitives.ReadUInt16LittleEndian(coff[2..]);
        var timestamp = BinaryPrimitives.ReadUInt32LittleEndian(coff[4..]);
        var optionalSize = BinaryPrimitives.ReadUInt16LittleEndian(coff[16..]);
        var optionalOffset = peOffset + 24;
        if (sectionCount == 0 || optionalSize < 0x70 ||
            optionalOffset > image.Length - optionalSize ||
            BinaryPrimitives.ReadUInt16LittleEndian(image.AsSpan(optionalOffset)) != Pe32PlusMagic)
        {
            return false;
        }

        var sizeOfImage = BinaryPrimitives.ReadUInt32LittleEndian(image.AsSpan(optionalOffset + 0x38));
        var sectionOffset = optionalOffset + optionalSize;
        if (sectionOffset > image.Length - checked(sectionCount * 40))
        {
            return false;
        }

        headers = new PeHeaders(machine, timestamp, sizeOfImage, sectionOffset, sectionCount);
        return true;
    }

    private static bool TryMapExecutableWindow(
        byte[] image,
        PeHeaders headers,
        uint rva,
        int length,
        out ReadOnlySpan<byte> window)
    {
        window = default;
        if (length <= 0)
        {
            return false;
        }

        for (var index = 0; index < headers.SectionCount; index++)
        {
            var section = image.AsSpan(headers.SectionOffset + index * 40, 40);
            var virtualSize = BinaryPrimitives.ReadUInt32LittleEndian(section[8..]);
            var virtualAddress = BinaryPrimitives.ReadUInt32LittleEndian(section[12..]);
            var rawSize = BinaryPrimitives.ReadUInt32LittleEndian(section[16..]);
            var rawOffset = BinaryPrimitives.ReadUInt32LittleEndian(section[20..]);
            var characteristics = BinaryPrimitives.ReadUInt32LittleEndian(section[36..]);
            if ((characteristics & ExecutableSection) == 0 || rva < virtualAddress)
            {
                continue;
            }

            var relative = (ulong)rva - virtualAddress;
            var requestedEnd = relative + checked((uint)length);
            if (requestedEnd > Math.Min((ulong)virtualSize, rawSize))
            {
                continue;
            }

            var fileOffset = (ulong)rawOffset + relative;
            if (fileOffset + checked((uint)length) > (ulong)image.LongLength)
            {
                continue;
            }
            window = image.AsSpan(checked((int)fileOffset), length);
            return true;
        }

        return false;
    }

    private readonly record struct PeHeaders(
        ushort Machine,
        uint Timestamp,
        uint SizeOfImage,
        int SectionOffset,
        ushort SectionCount);
}
