using System.Buffers.Binary;
using System.Diagnostics;
using System.IO.Pipes;
using System.Security.Cryptography;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text;
using DSRRandomizer.Foundation.Safety;
using DSRRandomizer.Launcher.Native;
using DSRRandomizer.Launcher.Safety;

namespace DSRRandomizer.Launcher.Tests.Safety;

public sealed class RemoteDllInjectorIntegrationTests
{
    [Fact]
    public void CreateInitBlock_MarshalsVersionedCanonicalSavePathsAtExactOffsets()
    {
        var configuration = GuardConfiguration.Create(
            @"C:\fixture\DSRRandomizer.Runtime.dll",
            ProtocolVersion: 2,
            RequiredFlags: (ulong)(ProtectionFlags.Bootstrap |
                                   ProtectionFlags.SaveKnownFolder |
                                   ProtectionFlags.SaveFileIo),
            DiagnosticMode: true) with
        {
            SavePaths = new GuardSavePathConfiguration(
                @"C:\fixture\virtual-documents",
                @"C:\fixture\virtual-documents\NBGI\DARK SOULS REMASTERED\12345678901234567\DRAKS0005.sl2",
                @"C:\fixture\real-normal",
                @"C:\fixture\external",
                @"C:\fixture\external\DRAKS0005.rmm")
        };

        var block = RemoteDllInjector.CreateInitBlock(
            configuration,
            @"\\.\pipe\fixture");

        Assert.Equal(5428, block.Length);
        Assert.Equal((ushort)2, BinaryPrimitives.ReadUInt16LittleEndian(block.AsSpan(4)));
        Assert.Equal((ushort)5428, BinaryPrimitives.ReadUInt16LittleEndian(block.AsSpan(6)));
        Assert.Equal(7UL, BinaryPrimitives.ReadUInt64LittleEndian(block.AsSpan(8)));
        Assert.Equal(@"\\.\pipe\fixture", ReadFixedWide(block, 52));
        Assert.Equal(configuration.SavePaths.VirtualDocuments, ReadFixedWide(block, 308));
        Assert.Equal(configuration.SavePaths.VirtualLogicalSave, ReadFixedWide(block, 1332));
        Assert.Equal(configuration.SavePaths.RealSaveRoot, ReadFixedWide(block, 2356));
        Assert.Equal(configuration.SavePaths.ExternalSaveRoot, ReadFixedWide(block, 3380));
        Assert.Equal(configuration.SavePaths.DedicatedRmm, ReadFixedWide(block, 4404));
    }

    [Fact]
    public void CreateInitBlock_RequiresBothSaveFlagsAndCanonicalConfiguration()
    {
        var missingPaths = GuardConfiguration.Create(
            @"C:\fixture\DSRRandomizer.Runtime.dll",
            ProtocolVersion: 2,
            RequiredFlags: (ulong)(ProtectionFlags.Bootstrap |
                                   ProtectionFlags.SaveKnownFolder |
                                   ProtectionFlags.SaveFileIo),
            DiagnosticMode: false);
        var partialFlags = missingPaths with
        {
            RequiredFlags = (ulong)(ProtectionFlags.Bootstrap |
                                    ProtectionFlags.SaveFileIo),
            SavePaths = new GuardSavePathConfiguration(
                @"C:\fixture\virtual-documents",
                @"C:\fixture\virtual-documents\NBGI\DARK SOULS REMASTERED\12345678901234567\DRAKS0005.sl2",
                @"C:\fixture\real-normal",
                @"C:\fixture\external",
                @"C:\fixture\external\DRAKS0005.rmm")
        };

        Assert.Throws<ArgumentException>(
            () => RemoteDllInjector.CreateInitBlock(missingPaths, @"\\.\pipe\fixture"));
        Assert.Throws<ArgumentException>(
            () => RemoteDllInjector.CreateInitBlock(partialFlags, @"\\.\pipe\fixture"));
    }

    [Fact]
    public async Task InitializeAsync_ReturnsCompleteFixtureHandshakeBeforeResume()
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
            ProtocolVersion: 2,
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
            ProtocolVersion: 99,
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
    public async Task InitializeAsync_RemoteTimeoutTerminatesChildBeforeReturning()
    {
        var paths = FindNativeArtifacts();
        await using var child = await CreateSuspendedFixtureAsync(paths.FixturePath);
        var configuration = GuardConfiguration.Create(
            paths.StalledGuardPath,
            ProtocolVersion: 2,
            RequiredFlags: (ulong)ProtectionFlags.Bootstrap,
            DiagnosticMode: true) with
        {
            OperationTimeout = TimeSpan.FromMilliseconds(150)
        };

        var result = await new RemoteDllInjector().InitializeAsync(
            child,
            configuration,
            CancellationToken.None);

        Assert.False(result.Success);
        Assert.Equal("SAFETY_REMOTE_TIMEOUT", result.ErrorCode);
        Assert.True(await WaitForExitAsync(child.ProcessId, TimeSpan.FromMilliseconds(500)));
    }

    [Fact]
    public async Task InitializeAsync_RemoteCancellationTerminatesChildBeforeRethrowing()
    {
        var paths = FindNativeArtifacts();
        await using var child = await CreateSuspendedFixtureAsync(paths.FixturePath);
        var configuration = GuardConfiguration.Create(
            paths.StalledGuardPath,
            ProtocolVersion: 2,
            RequiredFlags: (ulong)ProtectionFlags.Bootstrap,
            DiagnosticMode: true);
        using var cancellation = new CancellationTokenSource(
            TimeSpan.FromMilliseconds(150));

        await Assert.ThrowsAsync<OperationCanceledException>(
            () => new RemoteDllInjector().InitializeAsync(
                child,
                configuration,
                cancellation.Token));

        Assert.True(await WaitForExitAsync(child.ProcessId, TimeSpan.FromMilliseconds(500)));
    }

    [Fact]
    public async Task Coordinator_DoesNotInheritArbitraryParentEnvironment()
    {
        var paths = FindNativeArtifacts();
        const string sentinelName = "DSR_RANDOMIZER_PARENT_SENTINEL";
        var previousValue = Environment.GetEnvironmentVariable(sentinelName);
        Environment.SetEnvironmentVariable(sentinelName, "must-not-reach-child");
        var request = new SafetyLaunchRequest(
            paths.FixturePath,
            Path.GetDirectoryName(paths.FixturePath)!,
            paths.GuardPath,
            SupportedProfile(),
            RequiredProtectionFlags: (ulong)ProtectionFlags.Bootstrap,
            DiagnosticMode: true);

        try
        {
            var result = await new SafetyLaunchCoordinator(new WindowsProtectedProcessPlatform())
                .LaunchAsync(request, CancellationToken.None);

            Assert.True(result.Started, result.ErrorCode);
            Assert.Equal(0, result.ExitCode);
        }
        finally
        {
            Environment.SetEnvironmentVariable(sentinelName, previousValue);
        }
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
            var stalledGuardPath = Path.Combine(
                buildRoot,
                configuration,
                "DSRRandomizer.StalledRuntime.dll");
            if (File.Exists(fixturePath) &&
                File.Exists(guardPath) &&
                File.Exists(stalledGuardPath))
            {
                return new NativeArtifacts(fixturePath, guardPath, stalledGuardPath);
            }
        }

        throw new Xunit.Sdk.XunitException("Native fixture and guard DLL must be built first.");
    }

    private static async Task<bool> WaitForExitAsync(int processId, TimeSpan timeout)
    {
        var stopwatch = Stopwatch.StartNew();
        while (stopwatch.Elapsed < timeout)
        {
            try
            {
                using var process = Process.GetProcessById(processId);
                if (process.HasExited)
                {
                    return true;
                }
            }
            catch (ArgumentException)
            {
                return true;
            }

            await Task.Delay(TimeSpan.FromMilliseconds(25));
        }

        return false;
    }

    private static string ReadFixedWide(byte[] block, int offset)
    {
        var value = Encoding.Unicode.GetString(block, offset, 512 * sizeof(char));
        return value[..value.IndexOf('\0')];
    }

    private sealed record NativeArtifacts(
        string FixturePath,
        string GuardPath,
        string StalledGuardPath);
}
