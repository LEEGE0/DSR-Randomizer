using System.Text.Json;
using DSRRandomizer.Foundation.Paths;

namespace DSRRandomizer.Foundation.Runtime;

public sealed record RuntimePointer(
    string RuntimeId,
    string RelativeRuntimePath,
    string ManifestSha256);

public sealed class RuntimePointerStore
{
    private readonly LocalDataLayout _layout;
    private readonly WriteBoundary _boundary;

    public RuntimePointerStore(LocalDataLayout layout, WriteBoundary boundary)
    {
        _layout = layout;
        _boundary = boundary;
    }

    public async Task<RuntimePointer?> ReadAsync(CancellationToken cancellationToken)
    {
        var currentPath = Path.Combine(_layout.Root, "runtime-current.json");
        if (!File.Exists(currentPath))
        {
            return null;
        }

        await using var stream = File.OpenRead(currentPath);
        return await JsonSerializer.DeserializeAsync<RuntimePointer>(
            stream,
            RuntimeJson.Options,
            cancellationToken);
    }

    public async Task ActivateAsync(
        RuntimePointer pointer,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(pointer);
        _ = RuntimePathSafety.ResolveUnderRoot(_layout.Root, pointer.RelativeRuntimePath);

        var currentPath = Path.Combine(_layout.Root, "runtime-current.json");
        var temporaryPath = Path.Combine(
            _layout.Root,
            $"runtime-current.{Guid.NewGuid():N}.tmp");
        _boundary.EnsureAllowed(_layout.Root);
        _boundary.EnsureAllowed(currentPath);
        _boundary.EnsureAllowed(temporaryPath);
        Directory.CreateDirectory(_layout.Root);

        try
        {
            var bytes = JsonSerializer.SerializeToUtf8Bytes(pointer, RuntimeJson.Options);
            await File.WriteAllBytesAsync(temporaryPath, bytes, cancellationToken);
            File.Move(temporaryPath, currentPath, overwrite: true);
        }
        finally
        {
            if (File.Exists(temporaryPath))
            {
                File.Delete(temporaryPath);
            }
        }
    }
}
