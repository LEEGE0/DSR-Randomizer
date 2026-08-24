namespace DSRRandomizer.Foundation.Saves;

public sealed record FileIdentityAndHash(
    string Identity,
    long Length,
    DateTime LastWriteTimeUtc,
    string Sha256);

public interface IFileAccess
{
    bool Exists(string path);

    void CreateDirectory(string path);

    FileAttributes GetAttributes(string path);

    Stream Open(string path, FileMode mode, FileAccess access, FileShare share);

    Task<FileIdentityAndHash> IdentityAndHashAsync(
        Stream stream,
        CancellationToken cancellationToken);

    Task<FileIdentityAndHash> IdentityAndHashAsync(
        string path,
        CancellationToken cancellationToken);

    Task CopyAndFlushAsync(
        Stream source,
        string destinationPath,
        CancellationToken cancellationToken);

    Task WriteAllBytesAndFlushAsync(
        string path,
        ReadOnlyMemory<byte> bytes,
        CancellationToken cancellationToken);

    void MoveCreateNew(string sourcePath, string destinationPath);

    void Replace(string sourcePath, string destinationPath);

    void Delete(string path);
}
