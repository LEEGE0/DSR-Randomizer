namespace DSRRandomizer.Foundation.Runtime;

public sealed class DriveDiskSpaceProbe : IDiskSpaceProbe
{
    public long GetAvailableBytes(string path)
    {
        var root = Path.GetPathRoot(Path.GetFullPath(path));
        if (string.IsNullOrEmpty(root))
        {
            throw new IOException($"Unable to determine the destination drive for: {path}");
        }

        return new DriveInfo(root).AvailableFreeSpace;
    }
}
