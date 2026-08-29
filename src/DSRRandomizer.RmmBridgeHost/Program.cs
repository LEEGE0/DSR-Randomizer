using DSRRandomizer.Foundation.Installation;
using DSRRandomizer.Foundation.Paths;
using DSRRandomizer.Foundation.Saves;
using DSRRandomizer.RmmBridgeHost;

try
{
    var arguments = BridgeHostArguments.Parse(args);
    var canonicalizer = new WindowsPathCanonicalizer();
    var sourceStore = InstallationSelectionStore.CreateReadOnly(
        arguments.ExternalRoot, canonicalizer);
    var sourceInstallation = await sourceStore.ReadAsync(CancellationToken.None)
        ?? throw new InvalidDataException("The verified source-installation selection is missing.");
    var boundary = WriteBoundary.Create(
        sourceInstallation, arguments.ExternalRoot, canonicalizer);
    var layout = LocalDataLayout.Create(arguments.ExternalRoot, boundary);
    var service = new DedicatedSaveService(
        layout,
        boundary,
        new SaveSelectionStore(layout, boundary),
        new SystemFileAccess());
    var coordinator = new BridgeSessionCoordinator(
        new WindowsBridgeHostPlatform(),
        new DedicatedBridgeSaveSession(service));
    return await coordinator.RunAsync(arguments, CancellationToken.None);
}
catch
{
    return 20;
}
