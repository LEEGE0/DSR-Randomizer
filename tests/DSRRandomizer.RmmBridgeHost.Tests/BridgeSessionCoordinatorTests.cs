using DSRRandomizer.Foundation.Saves;
using DSRRandomizer.RmmBridgeHost;
using DSRRandomizer.RmmBridgeHost.GameParam;

namespace DSRRandomizer.RmmBridgeHost.Tests;

public sealed class BridgeSessionCoordinatorTests
{
    [Fact]
    public async Task RunAsync_SignalsOnlyAfterUncleanSessionAndCompletesNormalExit()
    {
        var calls = new List<string>();
        var platform = new FakePlatform(calls) { ExitCode = 0 };
        var sessions = new FakeSessions(calls);
        var coordinator = new BridgeSessionCoordinator(platform, new FakePublisher(calls), sessions);

        var exitCode = await coordinator.RunAsync(Arguments(), CancellationToken.None);

        Assert.Equal(0, exitCode);
        Assert.Equal(
            ["validate", "publish", "prepare", "begin", "signal", "wait", "complete:true"],
            calls);
    }

    [Fact]
    public async Task RunAsync_InvalidProcessBinding_NeverSignalsReady()
    {
        var calls = new List<string>();
        var platform = new FakePlatform(calls) { BindingValid = false };
        var coordinator = new BridgeSessionCoordinator(
            platform, new FakePublisher(calls), new FakeSessions(calls));

        var exitCode = await coordinator.RunAsync(Arguments(), CancellationToken.None);

        Assert.NotEqual(0, exitCode);
        Assert.Equal(["validate"], calls);
    }

    [Fact]
    public async Task RunAsync_NonzeroGameExit_CompletesAbnormally()
    {
        var calls = new List<string>();
        var platform = new FakePlatform(calls) { ExitCode = 17 };
        var coordinator = new BridgeSessionCoordinator(
            platform, new FakePublisher(calls), new FakeSessions(calls));

        var exitCode = await coordinator.RunAsync(Arguments(), CancellationToken.None);

        Assert.Equal(0, exitCode);
        Assert.Equal("complete:false", calls[^1]);
    }

    [Fact]
    public async Task RunAsync_ReadySignalFailure_ReleasesSessionAsAbnormal()
    {
        var calls = new List<string>();
        var platform = new FakePlatform(calls) { ThrowOnSignal = true };
        var coordinator = new BridgeSessionCoordinator(
            platform, new FakePublisher(calls), new FakeSessions(calls));

        var exitCode = await coordinator.RunAsync(Arguments(), CancellationToken.None);

        Assert.NotEqual(0, exitCode);
        Assert.Equal("complete:false", calls[^1]);
    }

    [Fact]
    public async Task RunAsync_PublicationFailure_ReturnsDistinctCodeBeforeSaveMutationOrReadiness()
    {
        var calls = new List<string>();
        var publisher = new FakePublisher(calls) { Throw = true };
        var coordinator = new BridgeSessionCoordinator(
            new FakePlatform(calls), publisher, new FakeSessions(calls));

        var exitCode = await coordinator.RunAsync(Arguments(), CancellationToken.None);

        Assert.Equal(15, exitCode);
        Assert.Equal(["validate", "publish"], calls);
    }

    private static BridgeHostArguments Arguments() => new(
        4242,
        @"D:\DSR MOD",
        "runtime-a39cb5e0",
        "424242424",
        @"Local\DSRRandomizer.RmmBridge.0123456789abcdef0123456789abcdef");

    private sealed class FakePlatform(List<string> calls) : IBridgeHostPlatform
    {
        public bool BindingValid { get; init; } = true;
        public uint ExitCode { get; init; }
        public bool ThrowOnSignal { get; init; }

        public BridgeBindingResult ValidateBinding(BridgeHostArguments arguments)
        {
            calls.Add("validate");
            return BindingValid
                ? BridgeBindingResult.Success()
                : BridgeBindingResult.Failure("mismatch");
        }

        public void SignalReady(string eventName)
        {
            calls.Add("signal");
            if (ThrowOnSignal)
            {
                throw new InvalidOperationException("signal failed");
            }
        }

        public Task<uint> WaitForExitAsync(uint processId, CancellationToken cancellationToken)
        {
            calls.Add("wait");
            return Task.FromResult(ExitCode);
        }
    }

    private sealed class FakeSessions(List<string> calls) : IBridgeSaveSession
    {
        public Task<DedicatedSaveResult> PrepareAsync(
            string steamId,
            CancellationToken cancellationToken)
        {
            calls.Add("prepare");
            return Task.FromResult(new DedicatedSaveResult(
                true, true, @"D:\DSR MOD\saves\424242424\DRAKS0005.rmm",
                SaveErrorCode.None, "")
            {
                SaveIdentity = "save-id",
                MetadataIdentity = "metadata-id"
            });
        }

        public Task<DedicatedSaveSessionResult> BeginSessionAsync(
            string steamId,
            string saveIdentity,
            string metadataIdentity,
            CancellationToken cancellationToken)
        {
            calls.Add("begin");
            return Task.FromResult(new DedicatedSaveSessionResult(
                new DedicatedSaveResult(true, true, @"D:\save.rmm", SaveErrorCode.None, ""),
                "session-token"));
        }

        public Task<DedicatedSaveResult> CompleteSessionAsync(
            string steamId,
            string sessionToken,
            bool normalGuardedExit,
            CancellationToken cancellationToken)
        {
            calls.Add($"complete:{normalGuardedExit.ToString().ToLowerInvariant()}");
            return Task.FromResult(new DedicatedSaveResult(
                normalGuardedExit, true, @"D:\save.rmm",
                normalGuardedExit ? SaveErrorCode.None : SaveErrorCode.ExistingSaveInvalid,
                ""));
        }
    }

    private sealed class FakePublisher(List<string> calls) : IGameParamPublisher
    {
        public bool Throw { get; init; }

        public Task PublishAsync(CancellationToken cancellationToken)
        {
            calls.Add("publish");
            if (Throw)
                throw new InvalidDataException("publisher failed");
            return Task.CompletedTask;
        }
    }
}
