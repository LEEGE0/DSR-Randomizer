namespace DSRRandomizer.Foundation.Installation;

public sealed record VerificationResult(
    bool IsValid,
    string CanonicalInstallationPath,
    GameFileCatalog? Catalog,
    IReadOnlyList<string> Errors);
