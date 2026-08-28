using System.Buffers.Binary;
using System.IO.MemoryMappedFiles;
using System.Security.Cryptography;
using System.Text;

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
    ModuleMissing,
    ModuleDeclarationMismatch
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
            var modules = new Dictionary<string, byte[]>(StringComparer.OrdinalIgnoreCase);
            foreach (var module in profile.Modules)
            {
                var modulePath = Path.Combine(directory, module.Name);
                if (!File.Exists(modulePath) ||
                    !string.Equals(Path.GetFileName(modulePath), module.Name,
                        StringComparison.OrdinalIgnoreCase))
                {
                    return new(ProfileError.ModuleMissing);
                }
                modules.Add(module.Name, ReadMappedFile(modulePath));
            }
            return VerifyFiles(executable, modules, profile);
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
        return InspectIdentity(ReadMappedFile(path));
    }

    public static ExecutableIdentity InspectIdentity(byte[] image)
    {
        ArgumentNullException.ThrowIfNull(image);
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

    public static ProfileVerificationResult VerifyFiles(
        byte[] executable,
        IReadOnlyDictionary<string, byte[]> modules,
        CompatibilityProfile profile)
    {
        ArgumentNullException.ThrowIfNull(executable);
        ArgumentNullException.ThrowIfNull(modules);
        ArgumentNullException.ThrowIfNull(profile);

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
            if (!modules.TryGetValue(module.Name, out var image))
            {
                return new(ProfileError.ModuleMissing);
            }
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
            if (module.DeclaredInterfaces.Count != 0
                || module.ProtectedFactoryExports.Count != 0)
            {
                if (module.DeclaredInterfaces.Any(version =>
                        !ContainsNullTerminatedAscii(executable, version))
                    || module.ProtectedFactoryExports.Any(name =>
                        !HasExport(image, name))
                    || HasImport(executable, module.Name) == module.AllowDeferred)
                {
                    return new(ProfileError.ModuleDeclarationMismatch);
                }
            }
        }

        return ProfileVerificationResult.Success;
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
        if (sectionCount == 0 || optionalSize < 0x80 ||
            optionalOffset > image.Length - optionalSize ||
            BinaryPrimitives.ReadUInt16LittleEndian(image.AsSpan(optionalOffset)) != Pe32PlusMagic)
        {
            return false;
        }

        var sizeOfImage = BinaryPrimitives.ReadUInt32LittleEndian(image.AsSpan(optionalOffset + 0x38));
        var exportDirectoryRva = BinaryPrimitives.ReadUInt32LittleEndian(
            image.AsSpan(optionalOffset + 0x70));
        var importDirectoryRva = BinaryPrimitives.ReadUInt32LittleEndian(
            image.AsSpan(optionalOffset + 0x78));
        var sectionOffset = optionalOffset + optionalSize;
        if (sectionOffset > image.Length - checked(sectionCount * 40))
        {
            return false;
        }

        headers = new PeHeaders(
            machine,
            timestamp,
            sizeOfImage,
            sectionOffset,
            sectionCount,
            exportDirectoryRva,
            importDirectoryRva);
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

    private static bool ContainsNullTerminatedAscii(byte[] image, string value)
    {
        var needle = Encoding.ASCII.GetBytes(value + '\0');
        return image.AsSpan().IndexOf(needle) >= 0;
    }

    private static bool HasImport(byte[] image, string moduleName)
    {
        if (!TryReadHeaders(image, out var headers) ||
            headers.ImportDirectoryRva == 0 ||
            !TryMapRva(image, headers, headers.ImportDirectoryRva, 20, out var offset))
        {
            return false;
        }
        for (var index = offset; index <= image.Length - 20; index += 20)
        {
            var descriptor = image.AsSpan(index, 20);
            if (descriptor.SequenceEqual(new byte[20]))
            {
                return false;
            }
            var nameRva = BinaryPrimitives.ReadUInt32LittleEndian(descriptor[12..]);
            if (TryReadAscii(image, headers, nameRva, out var name) &&
                string.Equals(name, moduleName, StringComparison.OrdinalIgnoreCase))
            {
                return true;
            }
        }
        return false;
    }

    private static bool HasExport(byte[] image, string exportName)
    {
        if (!TryReadHeaders(image, out var headers) ||
            headers.ExportDirectoryRva == 0 ||
            !TryMapRva(image, headers, headers.ExportDirectoryRva, 40, out var offset))
        {
            return false;
        }
        var directory = image.AsSpan(offset, 40);
        var count = BinaryPrimitives.ReadUInt32LittleEndian(directory[24..]);
        var namesRva = BinaryPrimitives.ReadUInt32LittleEndian(directory[32..]);
        if (count > 65536 || !TryMapRva(
                image, headers, namesRva, checked((int)count * 4), out var namesOffset))
        {
            return false;
        }
        for (var index = 0; index < count; index++)
        {
            var nameRva = BinaryPrimitives.ReadUInt32LittleEndian(
                image.AsSpan(namesOffset + checked((int)index * 4), 4));
            if (TryReadAscii(image, headers, nameRva, out var name) &&
                string.Equals(name, exportName, StringComparison.Ordinal))
            {
                return true;
            }
        }
        return false;
    }

    private static bool TryReadAscii(
        byte[] image,
        PeHeaders headers,
        uint rva,
        out string value)
    {
        value = string.Empty;
        if (!TryMapRva(image, headers, rva, 1, out var offset))
        {
            return false;
        }
        var end = Array.IndexOf(image, (byte)0, offset);
        if (end < offset || end - offset > 4096)
        {
            return false;
        }
        value = Encoding.ASCII.GetString(image, offset, end - offset);
        return true;
    }

    private static bool TryMapRva(
        byte[] image,
        PeHeaders headers,
        uint rva,
        int length,
        out int fileOffset)
    {
        fileOffset = 0;
        if (length < 0)
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
            if (rva < virtualAddress)
            {
                continue;
            }
            var relative = (ulong)rva - virtualAddress;
            if (relative + checked((uint)length) > Math.Min((ulong)virtualSize, rawSize) ||
                rawOffset + relative + checked((uint)length) > (ulong)image.Length)
            {
                continue;
            }
            fileOffset = checked((int)(rawOffset + relative));
            return true;
        }
        return false;
    }

    private readonly record struct PeHeaders(
        ushort Machine,
        uint Timestamp,
        uint SizeOfImage,
        int SectionOffset,
        ushort SectionCount,
        uint ExportDirectoryRva,
        uint ImportDirectoryRva);
}
