using DSRRandomizer.Launcher.Native;

namespace DSRRandomizer.Launcher.Tests.Native;

public sealed class WindowsProtectedProcessPlatformTests
{
    [Fact]
    public void CreateMinimalEnvironmentBlock_IncludesDarkSoulsRemasteredSteamIdentity()
    {
        var entries = WindowsProtectedProcessPlatform
            .CreateMinimalEnvironmentBlock()
            .Split('\0', StringSplitOptions.RemoveEmptyEntries);

        Assert.Contains("SteamAppId=570940", entries);
        Assert.Contains("SteamGameId=570940", entries);
    }
}
