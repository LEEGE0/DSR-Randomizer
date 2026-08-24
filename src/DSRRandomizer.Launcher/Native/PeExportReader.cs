using System.Buffers.Binary;
using System.Text;
using DSRRandomizer.Launcher.Safety;

namespace DSRRandomizer.Launcher.Native;

internal static class PeExportReader
{
    private const ushort DosSignature = 0x5A4D;
    private const uint PeSignature = 0x00004550;
    private const ushort Pe32PlusMagic = 0x020B;
    private const int SectionHeaderSize = 40;

    internal static uint ReadExportRva(string path, string exportName)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        ArgumentException.ThrowIfNullOrWhiteSpace(exportName);

        try
        {
            var image = File.ReadAllBytes(path);
            return ReadExportRva(image, exportName);
        }
        catch (SafetyLaunchException)
        {
            throw;
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            throw new SafetyLaunchException("SAFETY_GUARD_EXPORT_INVALID");
        }
    }

    private static uint ReadExportRva(ReadOnlySpan<byte> image, string exportName)
    {
        RequireRange(image, 0, 64);
        if (ReadUInt16(image, 0) != DosSignature)
        {
            throw InvalidImage();
        }

        var peOffset = checked((int)ReadUInt32(image, 0x3C));
        RequireRange(image, peOffset, 24);
        if (ReadUInt32(image, peOffset) != PeSignature)
        {
            throw InvalidImage();
        }

        var sectionCount = ReadUInt16(image, peOffset + 6);
        var optionalHeaderSize = ReadUInt16(image, peOffset + 20);
        var optionalHeaderOffset = peOffset + 24;
        RequireRange(image, optionalHeaderOffset, optionalHeaderSize);
        if (ReadUInt16(image, optionalHeaderOffset) != Pe32PlusMagic || optionalHeaderSize < 120)
        {
            throw InvalidImage();
        }

        var exportRva = ReadUInt32(image, optionalHeaderOffset + 112);
        var exportSize = ReadUInt32(image, optionalHeaderOffset + 116);
        if (exportRva == 0 || exportSize == 0)
        {
            throw MissingExport();
        }

        var sectionTableOffset = optionalHeaderOffset + optionalHeaderSize;
        RequireRange(image, sectionTableOffset, checked(sectionCount * SectionHeaderSize));
        var sections = ReadSections(image, sectionTableOffset, sectionCount);
        var exportOffset = RvaToOffset(image, sections, exportRva);
        RequireRange(image, exportOffset, 40);

        var functionCount = ReadUInt32(image, exportOffset + 20);
        var nameCount = ReadUInt32(image, exportOffset + 24);
        var functionTable = ReadUInt32(image, exportOffset + 28);
        var nameTable = ReadUInt32(image, exportOffset + 32);
        var ordinalTable = ReadUInt32(image, exportOffset + 36);
        if (functionCount == 0 || nameCount == 0)
        {
            throw MissingExport();
        }

        var nameTableOffset = RvaToOffset(image, sections, nameTable);
        var ordinalTableOffset = RvaToOffset(image, sections, ordinalTable);
        var functionTableOffset = RvaToOffset(image, sections, functionTable);
        RequireRange(image, nameTableOffset, checked((int)nameCount * 4));
        RequireRange(image, ordinalTableOffset, checked((int)nameCount * 2));
        RequireRange(image, functionTableOffset, checked((int)functionCount * 4));

        for (var index = 0; index < nameCount; index++)
        {
            var nameRva = ReadUInt32(image, nameTableOffset + checked((int)index * 4));
            var nameOffset = RvaToOffset(image, sections, nameRva);
            if (!string.Equals(ReadAsciiZ(image, nameOffset), exportName, StringComparison.Ordinal))
            {
                continue;
            }

            var ordinal = ReadUInt16(image, ordinalTableOffset + checked((int)index * 2));
            if (ordinal >= functionCount)
            {
                throw InvalidImage();
            }

            var functionRva = ReadUInt32(
                image,
                functionTableOffset + checked(ordinal * 4));
            if (functionRva >= exportRva && functionRva < checked(exportRva + exportSize))
            {
                throw new SafetyLaunchException("SAFETY_GUARD_FORWARDED_EXPORT");
            }

            return functionRva;
        }

        throw MissingExport();
    }

    private static Section[] ReadSections(
        ReadOnlySpan<byte> image,
        int offset,
        ushort count)
    {
        var sections = new Section[count];
        for (var index = 0; index < count; index++)
        {
            var sectionOffset = offset + (index * SectionHeaderSize);
            sections[index] = new Section(
                ReadUInt32(image, sectionOffset + 12),
                ReadUInt32(image, sectionOffset + 8),
                ReadUInt32(image, sectionOffset + 20),
                ReadUInt32(image, sectionOffset + 16));
        }

        return sections;
    }

    private static int RvaToOffset(
        ReadOnlySpan<byte> image,
        IEnumerable<Section> sections,
        uint rva)
    {
        foreach (var section in sections)
        {
            var length = Math.Max(section.VirtualSize, section.RawSize);
            if (rva < section.VirtualAddress ||
                (ulong)rva >= (ulong)section.VirtualAddress + length)
            {
                continue;
            }

            var offset = checked((int)(section.RawOffset + (rva - section.VirtualAddress)));
            RequireRange(image, offset, 1);
            return offset;
        }

        if (rva < image.Length)
        {
            return checked((int)rva);
        }

        throw InvalidImage();
    }

    private static string ReadAsciiZ(ReadOnlySpan<byte> image, int offset)
    {
        RequireRange(image, offset, 1);
        var remaining = image[offset..];
        var terminator = remaining.IndexOf((byte)0);
        if (terminator < 0)
        {
            throw InvalidImage();
        }

        return Encoding.ASCII.GetString(remaining[..terminator]);
    }

    private static ushort ReadUInt16(ReadOnlySpan<byte> image, int offset)
    {
        RequireRange(image, offset, sizeof(ushort));
        return BinaryPrimitives.ReadUInt16LittleEndian(image[offset..]);
    }

    private static uint ReadUInt32(ReadOnlySpan<byte> image, int offset)
    {
        RequireRange(image, offset, sizeof(uint));
        return BinaryPrimitives.ReadUInt32LittleEndian(image[offset..]);
    }

    private static void RequireRange(ReadOnlySpan<byte> image, int offset, int length)
    {
        if (offset < 0 || length < 0 || offset > image.Length - length)
        {
            throw InvalidImage();
        }
    }

    private static SafetyLaunchException InvalidImage() =>
        new("SAFETY_GUARD_EXPORT_INVALID");

    private static SafetyLaunchException MissingExport() =>
        new("SAFETY_GUARD_EXPORT_MISSING");

    private sealed record Section(
        uint VirtualAddress,
        uint VirtualSize,
        uint RawOffset,
        uint RawSize);
}
