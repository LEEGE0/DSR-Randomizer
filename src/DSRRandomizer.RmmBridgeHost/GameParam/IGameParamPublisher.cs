namespace DSRRandomizer.RmmBridgeHost.GameParam;

public interface IGameParamPublisher
{
    Task PublishAsync(CancellationToken cancellationToken);
}
