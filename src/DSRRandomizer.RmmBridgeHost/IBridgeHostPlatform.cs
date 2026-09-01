namespace DSRRandomizer.RmmBridgeHost;

public sealed record BridgeBindingResult(bool Valid, string Message)
{
    public static BridgeBindingResult Success() => new(true, string.Empty);
    public static BridgeBindingResult Failure(string message) => new(false, message);
}

public interface IBridgeHostPlatform
{
    BridgeBindingResult ValidateBinding(BridgeHostArguments arguments);
    void SignalReady(string eventName);
    Task<uint> WaitForExitAsync(uint processId, CancellationToken cancellationToken);
}
