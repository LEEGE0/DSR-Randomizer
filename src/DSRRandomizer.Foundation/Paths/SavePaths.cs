using System.Text.RegularExpressions;

namespace DSRRandomizer.Foundation.Paths;

public static partial class SavePaths
{
    public static string GetDedicatedSave(string localDataRoot, string steamId, WriteBoundary boundary)
    {
        ArgumentNullException.ThrowIfNull(boundary);

        var destination = ResolveDedicatedSave(localDataRoot, steamId);
        boundary.EnsureAllowed(destination);
        return destination;
    }

    private static string ResolveDedicatedSave(string localDataRoot, string steamId)
    {
        ValidateSteamId(steamId);

        var root = Path.GetFullPath(localDataRoot);
        return Path.GetFullPath(Path.Combine(root, "saves", steamId, "DRAKS0005.rmm"));
    }

    private static void ValidateSteamId(string steamId)
    {
        if (string.IsNullOrWhiteSpace(steamId) || !SteamIdPattern().IsMatch(steamId))
        {
            throw new ArgumentException("Steam ID must contain 16 to 20 decimal digits.", nameof(steamId));
        }
    }

    [GeneratedRegex("^[0-9]{16,20}$", RegexOptions.CultureInvariant)]
    private static partial Regex SteamIdPattern();
}
