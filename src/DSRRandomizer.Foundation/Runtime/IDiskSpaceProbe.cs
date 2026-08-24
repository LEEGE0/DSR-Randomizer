namespace DSRRandomizer.Foundation.Runtime;

public interface IDiskSpaceProbe
{
    long GetAvailableBytes(string path);
}
