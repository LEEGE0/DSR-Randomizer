using DSRRandomizer.Foundation.Safety;

namespace DSRRandomizer.Foundation.Tests.Safety;

public sealed class CompatibilityProfileCatalogTests
{
    private const string SupportedHash =
        "a45aaa36dd2f6cc151670a639ea5547043cf38ea79ff4178b963c6ed71f98d7b";

    [Fact]
    public void Select_ReturnsOnlyTheExactSupportedIdentity()
    {
        var identity = new ExecutableIdentity(
            50286344,
            SupportedHash.ToUpperInvariant(),
            0x8664,
            0x6344ca56,
            52015104);

        var profile = CompatibilityProfileCatalog.Default.Select(identity);

        Assert.Equal("dsr-steam-a45aaa36", profile.Id);
        Assert.Equal(4326608, profile.FixedSaveLength);
        Assert.Equal(1, profile.ProtocolVersion);
    }

    [Theory]
    [InlineData(1, 0x8664, 0x6344ca56, 52015104)]
    [InlineData(50286344, 0x014c, 0x6344ca56, 52015104)]
    [InlineData(50286344, 0x8664, 0x00000000, 52015104)]
    [InlineData(50286344, 0x8664, 0x6344ca56, 1)]
    public void Select_RejectsAnyNonHashIdentityMismatch(
        long length,
        ushort machine,
        uint peTimestamp,
        uint sizeOfImage)
    {
        var identity = new ExecutableIdentity(
            length,
            SupportedHash,
            machine,
            peTimestamp,
            sizeOfImage);

        Assert.Throws<UnsupportedGameBuildException>(
            () => CompatibilityProfileCatalog.Default.Select(identity));
    }

    [Fact]
    public void Select_RejectsWrongOrMalformedHash()
    {
        var wrongHash = new string('0', 64);
        var identity = SupportedIdentity() with { Sha256 = wrongHash };

        Assert.Throws<UnsupportedGameBuildException>(
            () => CompatibilityProfileCatalog.Default.Select(identity));
        Assert.Throws<ArgumentException>(
            () => CompatibilityProfileCatalog.Default.Select(
                identity with { Sha256 = "not-a-sha256" }));
    }

    [Fact]
    public void Constructor_RejectsDuplicateExecutableIdentity()
    {
        var identity = SupportedIdentity();
        var profiles = new[]
        {
            new CompatibilityProfile("first", identity, 4326608, 1),
            new CompatibilityProfile("second", identity with { Sha256 = SupportedHash.ToUpperInvariant() }, 4326608, 1)
        };

        Assert.Throws<ArgumentException>(() => new CompatibilityProfileCatalog(profiles));
    }

    private static ExecutableIdentity SupportedIdentity() => new(
        50286344,
        SupportedHash,
        0x8664,
        0x6344ca56,
        52015104);
}
