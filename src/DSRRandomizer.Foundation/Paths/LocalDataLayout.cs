namespace DSRRandomizer.Foundation.Paths;

public sealed record LocalDataLayout(
    string Root,
    string Runtimes,
    string Components,
    string VirtualProfile,
    string Staging,
    string ActiveSeed,
    string Saves,
    string Config,
    string Logs)
{
    public static LocalDataLayout Create(string localDataRoot, WriteBoundary boundary)
    {
        ArgumentNullException.ThrowIfNull(boundary);

        var root = Path.GetFullPath(localDataRoot).TrimEnd(Path.DirectorySeparatorChar);
        var layout = new LocalDataLayout(
            root,
            Path.Combine(root, "runtimes"),
            Path.Combine(root, "components"),
            Path.Combine(root, "profile"),
            Path.Combine(root, "staging"),
            Path.Combine(root, "active-seed"),
            Path.Combine(root, "saves"),
            Path.Combine(root, "config"),
            Path.Combine(root, "logs"));

        boundary.EnsureAllowed(layout.Root);
        boundary.EnsureAllowed(layout.Runtimes);
        boundary.EnsureAllowed(layout.Components);
        boundary.EnsureAllowed(layout.VirtualProfile);
        boundary.EnsureAllowed(layout.Staging);
        boundary.EnsureAllowed(layout.ActiveSeed);
        boundary.EnsureAllowed(layout.Saves);
        boundary.EnsureAllowed(layout.Config);
        boundary.EnsureAllowed(layout.Logs);
        return layout;
    }
}
