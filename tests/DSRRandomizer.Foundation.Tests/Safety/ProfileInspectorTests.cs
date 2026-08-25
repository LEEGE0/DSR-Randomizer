using System.Buffers.Binary;
using System.Security.Cryptography;
using DSRRandomizer.Foundation.Safety;

namespace DSRRandomizer.Foundation.Tests.Safety;

public sealed class ProfileInspectorTests
{
    private static readonly byte[] TargetBytes =
        [0x48, 0x8b, 0xc1, 0x48, 0x85, 0xc0, 0x74, 0x04, 0x33, 0xc0, 0xc3, 0x90, 0x90, 0x90, 0x90, 0x90];

    [Fact]
    public void VerifyImage_RejectsOneByteFingerprintChangeAfterExactIdentityMatch()
    {
        var image = SyntheticPe.Create(TargetBytes);
        var expectedFingerprint = Sha256(TargetBytes);
        image[0x400] ^= 1;
        var identity = SyntheticPe.Identity(image);
        var target = new InternalTargetProfile(
            "DarkSoulsRemastered.exe",
            0x1200,
            expectedFingerprint,
            TargetBytes.Length,
            InternalTargetAction.ForceOffline);

        var result = ProfileInspector.VerifyImage(
            image,
            "DarkSoulsRemastered.exe",
            identity,
            [target]);

        Assert.Equal(ProfileError.TargetFingerprintMismatch, result.Error);
    }

    [Fact]
    public void VerifyImage_RequiresEveryPeIdentityFieldAndExecutableRvaWindow()
    {
        var image = SyntheticPe.Create(TargetBytes);
        var identity = SyntheticPe.Identity(image);
        var target = new InternalTargetProfile(
            "DarkSoulsRemastered.exe",
            0x1200,
            Sha256(TargetBytes),
            TargetBytes.Length,
            InternalTargetAction.ForceOffline);

        ProfileError Verify(ExecutableIdentity expected, InternalTargetProfile candidate) =>
            ProfileInspector.VerifyImage(
                image, "DarkSoulsRemastered.exe", expected, [candidate]).Error;

        Assert.Equal(ProfileError.LengthMismatch,
            Verify(identity with { Length = identity.Length + 1 }, target));
        Assert.Equal(ProfileError.FileHashMismatch,
            Verify(identity with { Sha256 = new string('0', 64) }, target));
        Assert.Equal(ProfileError.MachineMismatch,
            Verify(identity with { Machine = 0x014c }, target));
        Assert.Equal(ProfileError.PeTimestampMismatch,
            Verify(identity with { PeTimestamp = identity.PeTimestamp + 1 }, target));
        Assert.Equal(ProfileError.ImageSizeMismatch,
            Verify(identity with { SizeOfImage = identity.SizeOfImage + 1 }, target));
        Assert.Equal(ProfileError.TargetOutOfRange,
            Verify(identity, target with { Rva = 0x2000 }));
    }

    [Fact]
    public void LoadJson_SchemaOneIsTheSingleSourceForTargetsAndModules()
    {
        var image = SyntheticPe.Create(TargetBytes);
        var identity = SyntheticPe.Identity(image);
        var json = ProfileJson(identity, schemaVersion: 1);

        var profile = CompatibilityProfileCatalog.LoadJson(json).Select(identity);

        Assert.Equal("DarkSoulsRemastered.exe", profile.ExecutableModule);
        var module = Assert.Single(profile.Modules);
        Assert.Equal("steam_api64.dll", module.Name);
        Assert.Equal(2, profile.GameServiceTargets.Count);
        Assert.Contains(profile.GameServiceTargets, target => target.Action == InternalTargetAction.ForceOffline);
        Assert.Contains(profile.GameServiceTargets, target => target.Action == InternalTargetAction.DenyCall);
    }

    [Fact]
    public void LoadJson_RejectsSchemaMismatchAndDuplicateProfileIds()
    {
        var identity = SyntheticPe.Identity(SyntheticPe.Create(TargetBytes));

        Assert.Throws<CompatibilityProfileFormatException>(
            () => CompatibilityProfileCatalog.LoadJson(ProfileJson(identity, schemaVersion: 2)));

        var profile = ProfileObject(identity);
        var duplicateJson = $$"""
            {"schemaVersion":1,"profiles":[{{profile}},{{profile}}]}
            """;
        Assert.Throws<CompatibilityProfileFormatException>(
            () => CompatibilityProfileCatalog.LoadJson(duplicateJson));
    }

    [Fact]
    public void VerifyFiles_MapsBothModulesReadOnlyAndRejectsWrongModuleHash()
    {
        using var fixture = new TemporaryDirectory();
        var executable = SyntheticPe.Create(TargetBytes);
        TargetBytes.CopyTo(executable, 0x420);
        var steam = SyntheticPe.Create(TargetBytes);
        var executablePath = Path.Combine(fixture.Path, "DarkSoulsRemastered.exe");
        var steamPath = Path.Combine(fixture.Path, "steam_api64.dll");
        File.WriteAllBytes(executablePath, executable);
        File.WriteAllBytes(steamPath, steam);
        var timestamp = DateTime.UtcNow.AddHours(-1);
        File.SetLastWriteTimeUtc(executablePath, timestamp);
        File.SetLastWriteTimeUtc(steamPath, timestamp);

        var profile = new CompatibilityProfile(
            "synthetic",
            "DarkSoulsRemastered.exe",
            SyntheticPe.Identity(executable),
            4326608,
            2,
            [new ModuleProfile("steam_api64.dll", SyntheticPe.Identity(steam))],
            [
                new InternalTargetProfile(
                    "DarkSoulsRemastered.exe", 0x1200, Sha256(TargetBytes),
                    TargetBytes.Length, InternalTargetAction.ForceOffline),
                new InternalTargetProfile(
                    "DarkSoulsRemastered.exe", 0x1220, Sha256(TargetBytes),
                    TargetBytes.Length, InternalTargetAction.DenyCall)
            ]);

        var result = ProfileInspector.VerifyFiles(executablePath, profile);

        Assert.Equal(ProfileError.None, result.Error);
        Assert.Equal(timestamp, File.GetLastWriteTimeUtc(executablePath));
        Assert.Equal(timestamp, File.GetLastWriteTimeUtc(steamPath));
        steam[0x400] ^= 1;
        File.WriteAllBytes(steamPath, steam);
        Assert.Equal(
            ProfileError.FileHashMismatch,
            ProfileInspector.VerifyFiles(executablePath, profile).Error);
    }

    private static string ProfileJson(ExecutableIdentity identity, int schemaVersion) =>
        $$"""
        {"schemaVersion":{{schemaVersion}},"profiles":[{{ProfileObject(identity)}}]}
        """;

    private static string ProfileObject(ExecutableIdentity identity) =>
        $$"""
        {"id":"synthetic","executableModule":"DarkSoulsRemastered.exe","executable":{"length":{{identity.Length}},"sha256":"{{identity.Sha256}}","machine":{{identity.Machine}},"peTimestamp":{{identity.PeTimestamp}},"sizeOfImage":{{identity.SizeOfImage}}},"fixedSaveLength":4326608,"protocolVersion":2,"modules":[{"name":"steam_api64.dll","length":{{identity.Length}},"sha256":"{{identity.Sha256}}","machine":{{identity.Machine}},"peTimestamp":{{identity.PeTimestamp}},"sizeOfImage":{{identity.SizeOfImage}}}],"gameServiceTargets":[{"module":"DarkSoulsRemastered.exe","rva":4608,"fingerprintSha256":"{{Sha256(TargetBytes)}}","patchLength":16,"action":"ForceOffline"},{"module":"DarkSoulsRemastered.exe","rva":4640,"fingerprintSha256":"{{Sha256(TargetBytes)}}","patchLength":16,"action":"DenyCall"}]}
        """;

    private static string Sha256(byte[] value) =>
        Convert.ToHexString(SHA256.HashData(value)).ToLowerInvariant();

    private static class SyntheticPe
    {
        internal static byte[] Create(byte[] targetBytes)
        {
            var image = new byte[0x1400];
            image[0] = (byte)'M';
            image[1] = (byte)'Z';
            BinaryPrimitives.WriteInt32LittleEndian(image.AsSpan(0x3c), 0x80);
            image[0x80] = (byte)'P';
            image[0x81] = (byte)'E';
            BinaryPrimitives.WriteUInt16LittleEndian(image.AsSpan(0x84), 0x8664);
            BinaryPrimitives.WriteUInt16LittleEndian(image.AsSpan(0x86), 1);
            BinaryPrimitives.WriteUInt32LittleEndian(image.AsSpan(0x88), 0x6344ca56);
            BinaryPrimitives.WriteUInt16LittleEndian(image.AsSpan(0x94), 0xf0);
            BinaryPrimitives.WriteUInt16LittleEndian(image.AsSpan(0x98), 0x20b);
            BinaryPrimitives.WriteUInt32LittleEndian(image.AsSpan(0xd0), 0x3000);
            BinaryPrimitives.WriteUInt32LittleEndian(image.AsSpan(0xd4), 0x200);

            var section = image.AsSpan(0x188);
            ".text\0\0\0"u8.CopyTo(section);
            BinaryPrimitives.WriteUInt32LittleEndian(section[8..], 0x1000);
            BinaryPrimitives.WriteUInt32LittleEndian(section[12..], 0x1000);
            BinaryPrimitives.WriteUInt32LittleEndian(section[16..], 0x1000);
            BinaryPrimitives.WriteUInt32LittleEndian(section[20..], 0x200);
            BinaryPrimitives.WriteUInt32LittleEndian(section[36..], 0x60000020);
            targetBytes.CopyTo(image, 0x400);
            return image;
        }

        internal static ExecutableIdentity Identity(byte[] image) => new(
            image.LongLength,
            Sha256(image),
            0x8664,
            0x6344ca56,
            0x3000);
    }

    private sealed class TemporaryDirectory : IDisposable
    {
        internal TemporaryDirectory()
        {
            Path = System.IO.Path.Combine(
                System.IO.Path.GetTempPath(),
                $"DSRRandomizer-ProfileInspector-{Guid.NewGuid():N}");
            Directory.CreateDirectory(Path);
        }

        internal string Path { get; }

        public void Dispose() => Directory.Delete(Path, recursive: true);
    }
}
