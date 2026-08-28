using DSRRandomizer.Foundation.Paths;

namespace DSRRandomizer.Foundation.Runtime;

public sealed class ModFolderDiscovery
{
    public IReadOnlyList<string> Discover(string runtimePath)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(runtimePath);

        var runtime = new DirectoryInfo(Path.GetFullPath(runtimePath));
        ThrowIfReparseDirectory(runtime);
        var mods = new DirectoryInfo(Path.Combine(runtime.FullName, LocalDataLayout.ModsDirectoryName));
        if (!mods.Exists)
        {
            return Array.Empty<string>();
        }

        ThrowIfReparseDirectory(mods);
        return mods.EnumerateDirectories()
            .Select(directory =>
            {
                ThrowIfReparseDirectory(directory);
                return directory.Name;
            })
            .Order(StringComparer.OrdinalIgnoreCase)
            .ToArray();
    }

    private static void ThrowIfReparseDirectory(DirectoryInfo directory)
    {
        if ((directory.Attributes & FileAttributes.ReparsePoint) != 0)
        {
            throw new IOException($"Reparse directories are not allowed: {directory.FullName}");
        }
    }
}
