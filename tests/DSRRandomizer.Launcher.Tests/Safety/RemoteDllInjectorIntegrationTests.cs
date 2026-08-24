using System.Diagnostics;
using System.IO.Pipes;
using System.Security.Cryptography;
using System.Security.AccessControl;
using System.Security.Principal;
using DSRRandomizer.Foundation.Safety;
using DSRRandomizer.Launcher.Native;
using DSRRandomizer.Launcher.Safety;

namespace DSRRandomizer.Launcher.Tests.Safety;

public sealed class RemoteDllInjectorIntegrationTests
{
    [Fact]
    public async Task InitializeAsync_ReturnsCompleteFixtureHandshakeBeforeResume()
    {
        var paths = FindNativeArtifacts();
        await using var child = await CreateSuspendedFixtureAsync(paths.FixturePath);
        var configuration = GuardConfiguration.Create(
            paths.GuardPath,
            ProtocolVersion: 1,
            RequiredFlags: (ulong)ProtectionFlags.Bootstrap,
            DiagnosticMode: true);

        var result = await new RemoteDllInjector().InitializeAsync(
            child,
            configuration,
            CancellationToken.None);

        Assert.True(result.Success, result.ErrorCode);
        Assert.Equal((ulong)ProtectionFlags.Bootstrap, result.ActiveFlags);
        using var fixture = Process.GetProcessById(child.ProcessId);
        Assert.False(fixture.HasExited);
    }

    [Fact]
    public async Task InitializeAsync_RejectsHandshakeWithWrongNonce()
    {
        var paths = FindNativeArtifacts();
        await using var child = await CreateSuspendedFixtureAsync(paths.FixturePath);
        var configuration = GuardConfiguration.Create(
            paths.GuardPath,
            ProtocolVersion: 1,
            RequiredFlags: (ulong)ProtectionFlags.Bootstrap,
            DiagnosticMode: true) with
        {
            InitializationNonce = RandomNumberGenerator.GetBytes(32)
        };

        var result = await new RemoteDllInjector().InitializeAsync(
            child,
            configuration,
            CancellationToken.None);

        Assert.False(result.Success);
        Assert.Equal("SAFETY_IPC_AUTH_FAILED", result.ErrorCode);
    }

    [Fact]
    public async Task WaitForHandshakeAsync_TimesOutWithoutClient()
    {
        await using var server = new ProtectionPipeServer(
            RandomNumberGenerator.GetBytes(32),
            TimeSpan.FromMilliseconds(100));

        var result = await server.WaitForHandshakeAsync(CancellationToken.None);

        Assert.False(result.Success);
        Assert.Equal("SAFETY_IPC_TIMEOUT", result.ErrorCode);
    }

    [Fact]
    public async Task PipeAcl_AllowsOnlyCurrentUserAndLocalSystem()
    {
        await using var server = new ProtectionPipeServer(
            RandomNumberGenerator.GetBytes(32),
            TimeSpan.FromSeconds(1));
        using var identity = WindowsIdentity.GetCurrent(TokenAccessLevels.Query);
        var expected = new[]
        {
            identity.User!.Value,
            new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null).Value
        }.Order(StringComparer.Ordinal).ToArray();

        var rules = server.GetAccessControlForTesting()
            .GetAccessRules(includeExplicit: true, includeInherited: false, typeof(SecurityIdentifier))
            .Cast<PipeAccessRule>()
            .ToArray();
        var actual = rules
            .Where(rule => rule.AccessControlType == AccessControlType.Allow)
            .Select(rule => rule.IdentityReference.Value)
            .Order(StringComparer.Ordinal)
            .ToArray();

        Assert.Equal(expected, actual);
        Assert.All(rules, rule => Assert.Equal(PipeAccessRights.FullControl, rule.PipeAccessRights));
    }

    [Fact]
    public async Task InitializeAsync_RejectsWrongProtocolBeforeResume()
    {
        var paths = FindNativeArtifacts();
        await using var child = await CreateSuspendedFixtureAsync(paths.FixturePath);
        var configuration = GuardConfiguration.Create(
            paths.GuardPath,
            ProtocolVersion: 2,
            RequiredFlags: (ulong)ProtectionFlags.Bootstrap,
            DiagnosticMode: true);

        var result = await new RemoteDllInjector().InitializeAsync(
            child,
            configuration,
            CancellationToken.None);

        Assert.False(result.Success);
        Assert.Equal("SAFETY_INITIALIZER_FAILED", result.ErrorCode);
        using var fixture = Process.GetProcessById(child.ProcessId);
        Assert.False(fixture.HasExited);
    }

    [Fact]
    public async Task Coordinator_InjectsAuthenticatesAndResumesFixtureExactlyOnce()
    {
        var paths = FindNativeArtifacts();
        var request = new SafetyLaunchRequest(
            paths.FixturePath,
            Path.GetDirectoryName(paths.FixturePath)!,
            paths.GuardPath,
            SupportedProfile(),
            RequiredProtectionFlags: (ulong)ProtectionFlags.Bootstrap,
            DiagnosticMode: true);

        var result = await new SafetyLaunchCoordinator(new WindowsProtectedProcessPlatform())
            .LaunchAsync(request, CancellationToken.None);

        Assert.True(result.Started, result.ErrorCode);
        Assert.Equal(2, result.ExitCode);
    }

    private static async Task<IProtectedProcess> CreateSuspendedFixtureAsync(string fixturePath)
    {
        var platform = new WindowsProtectedProcessPlatform();
        var child = await platform.CreateSuspendedAsync(
            new SafetyLaunchRequest(
                fixturePath,
                Path.GetDirectoryName(fixturePath)!,
                fixturePath,
                SupportedProfile(),
                RequiredProtectionFlags: (ulong)ProtectionFlags.Bootstrap,
                DiagnosticMode: true),
            CancellationToken.None);
        child.AssignKillOnCloseJob();
        return child;
    }

    private static CompatibilityProfile SupportedProfile() =>
        CompatibilityProfileCatalog.Default.Select(new ExecutableIdentity(
            50286344,
            "a45aaa36dd2f6cc151670a639ea5547043cf38ea79ff4178b963c6ed71f98d7b",
            0x8664,
            0x6344ca56,
            52015104));

    private static NativeArtifacts FindNativeArtifacts()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null &&
               !File.Exists(Path.Combine(directory.FullName, "DSR-Randomizer.sln")))
        {
            directory = directory.Parent;
        }

        Assert.NotNull(directory);
        foreach (var configuration in new[] { "Release", "Debug" })
        {
            var buildRoot = Path.Combine(
                directory.FullName,
                "native",
                "out",
                "build",
                $"windows-x64-{configuration.ToLowerInvariant()}",
                "native");
            var fixturePath = Path.Combine(
                buildRoot,
                configuration,
                "DSRRandomizer.SuspendedFixture.exe");
            var guardPath = Path.Combine(
                buildRoot,
                "runtime",
                configuration,
                "DSRRandomizer.Runtime.dll");
            if (File.Exists(fixturePath) && File.Exists(guardPath))
            {
                return new NativeArtifacts(fixturePath, guardPath);
            }
        }

        throw new Xunit.Sdk.XunitException("Native fixture and guard DLL must be built first.");
    }

    private sealed record NativeArtifacts(string FixturePath, string GuardPath);
}
