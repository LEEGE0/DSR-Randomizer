using DSRRandomizer.Foundation.Saves;
using DSRRandomizer.RmmBridgeHost.GameParam;

namespace DSRRandomizer.RmmBridgeHost;

public interface IBridgeSaveSession
{
    Task<DedicatedSaveResult> PrepareAsync(string steamId, CancellationToken cancellationToken);
    Task<DedicatedSaveSessionResult> BeginSessionAsync(
        string steamId,
        string saveIdentity,
        string metadataIdentity,
        CancellationToken cancellationToken);
    Task<DedicatedSaveResult> CompleteSessionAsync(
        string steamId,
        string sessionToken,
        bool normalGuardedExit,
        CancellationToken cancellationToken);
}

public sealed class DedicatedBridgeSaveSession(DedicatedSaveService service)
    : IBridgeSaveSession
{
    public Task<DedicatedSaveResult> PrepareAsync(
        string steamId,
        CancellationToken cancellationToken) =>
        service.PrepareAsync(steamId, cancellationToken);

    public Task<DedicatedSaveSessionResult> BeginSessionAsync(
        string steamId,
        string saveIdentity,
        string metadataIdentity,
        CancellationToken cancellationToken) =>
        service.BeginSessionAsync(
            steamId, saveIdentity, metadataIdentity, cancellationToken);

    public Task<DedicatedSaveResult> CompleteSessionAsync(
        string steamId,
        string sessionToken,
        bool normalGuardedExit,
        CancellationToken cancellationToken) =>
        service.CompleteSessionAsync(
            steamId, sessionToken, normalGuardedExit, cancellationToken);
}

public sealed class BridgeSessionCoordinator(
    IBridgeHostPlatform platform,
    IGameParamPublisher publisher,
    IBridgeSaveSession sessions)
{
    public async Task<int> RunAsync(
        BridgeHostArguments arguments,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(arguments);
        var binding = platform.ValidateBinding(arguments);
        if (!binding.Valid)
        {
            return 10;
        }

        try
        {
            await publisher.PublishAsync(cancellationToken).ConfigureAwait(false);
        }
        catch
        {
            return 15;
        }

        var prepared = await sessions.PrepareAsync(arguments.SteamId, cancellationToken)
            .ConfigureAwait(false);
        if (!prepared.Ready
            || string.IsNullOrWhiteSpace(prepared.SaveIdentity)
            || string.IsNullOrWhiteSpace(prepared.MetadataIdentity))
        {
            return 11;
        }

        var session = await sessions.BeginSessionAsync(
                arguments.SteamId,
                prepared.SaveIdentity,
                prepared.MetadataIdentity,
                cancellationToken)
            .ConfigureAwait(false);
        if (!session.Ready || string.IsNullOrWhiteSpace(session.SessionToken))
        {
            return 12;
        }

        try
        {
            platform.SignalReady(arguments.ReadyEventName);
            var gameExitCode = await platform.WaitForExitAsync(
                    arguments.GamePid, cancellationToken)
                .ConfigureAwait(false);
            var normalExit = gameExitCode == 0;
            var completion = await sessions.CompleteSessionAsync(
                    arguments.SteamId,
                    session.SessionToken,
                    normalExit,
                    CancellationToken.None)
                .ConfigureAwait(false);
            return normalExit && !completion.Ready ? 14 : 0;
        }
        catch
        {
            _ = await sessions.CompleteSessionAsync(
                    arguments.SteamId,
                    session.SessionToken,
                    normalGuardedExit: false,
                    CancellationToken.None)
                .ConfigureAwait(false);
            return 13;
        }
    }
}
