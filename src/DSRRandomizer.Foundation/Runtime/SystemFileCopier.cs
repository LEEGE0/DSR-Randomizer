namespace DSRRandomizer.Foundation.Runtime;

public sealed class SystemFileCopier : IFileCopier
{
    public void Copy(string source, string destination) =>
        File.Copy(source, destination, overwrite: false);
}
