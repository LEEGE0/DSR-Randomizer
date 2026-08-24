namespace DSRRandomizer.Foundation.Installation;

public sealed record GameFileEntry(
    string RelativePath,
    long Length,
    DateTime LastWriteTimeUtc);

public sealed record GameFileCatalog(
    IReadOnlyList<GameFileEntry> Files,
    long TotalBytes);
