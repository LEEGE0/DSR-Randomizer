namespace DSRRandomizer.Foundation.Safety;

public sealed record ExecutableIdentity(
    long Length,
    string Sha256,
    ushort Machine,
    uint PeTimestamp,
    uint SizeOfImage);
