using DSRRandomizer.RmmBridgeHost;

namespace DSRRandomizer.RmmBridgeHost.Tests;

public sealed class BridgeHostArgumentsTests
{
    [Fact]
    public void Parse_ValidStrictArguments_ReturnsContract()
    {
        var parsed = BridgeHostArguments.Parse([
            "--game-pid", "4242",
            "--external-root", @"D:\DSR MOD",
            "--runtime-id", "runtime-a39cb5e0",
            "--steam-id", "146808034",
            "--ready-event", @"Local\DSRRandomizer.RmmBridge.0123456789abcdef0123456789abcdef"
        ]);

        Assert.Equal((uint)4242, parsed.GamePid);
        Assert.Equal(@"D:\DSR MOD", parsed.ExternalRoot);
        Assert.Equal("runtime-a39cb5e0", parsed.RuntimeId);
        Assert.Equal("146808034", parsed.SteamId);
    }

    [Theory]
    [InlineData("--unknown", "value")]
    [InlineData("--game-pid", "0")]
    [InlineData("--steam-id", "not-digits")]
    [InlineData("--ready-event", "Global\\wrong")]
    public void Parse_InvalidOrDuplicateArguments_Throws(string switchName, string replacement)
    {
        var arguments = new[]
        {
            "--game-pid", "4242",
            "--external-root", @"D:\DSR MOD",
            "--runtime-id", "runtime-a39cb5e0",
            "--steam-id", "146808034",
            "--ready-event", @"Local\DSRRandomizer.RmmBridge.0123456789abcdef0123456789abcdef"
        };

        var index = Array.IndexOf(arguments, switchName);
        if (index >= 0)
        {
            arguments[index + 1] = replacement;
        }
        else
        {
            arguments = [.. arguments, switchName, replacement];
        }

        Assert.Throws<ArgumentException>(() => BridgeHostArguments.Parse(arguments));
    }
}
