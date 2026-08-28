using System.Text.RegularExpressions;

namespace DSRRandomizer.Foundation.Saves;

public interface ISaveProfileLocator
{
    Task<IReadOnlyList<SaveProfileCandidate>> DiscoverAsync(CancellationToken cancellationToken);
}

public sealed partial class WindowsSaveProfileLocator(IKnownFolderProvider knownFolderProvider) : ISaveProfileLocator
{
    private const string NormalSaveName = "DRAKS0005.sl2";

    public Task<IReadOnlyList<SaveProfileCandidate>> DiscoverAsync(CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(knownFolderProvider);
        cancellationToken.ThrowIfCancellationRequested();

        var saveRoot = Path.Combine(
            knownFolderProvider.GetDocumentsPath(),
            "NBGI",
            "DARK SOULS REMASTERED");
        if (!Directory.Exists(saveRoot))
        {
            return Task.FromResult<IReadOnlyList<SaveProfileCandidate>>([]);
        }

        var candidates = new List<SaveProfileCandidate>();
        foreach (var profileDirectory in Directory.EnumerateDirectories(saveRoot))
        {
            cancellationToken.ThrowIfCancellationRequested();

            var steamId = Path.GetFileName(profileDirectory);
            if (!SteamIdPattern().IsMatch(steamId))
            {
                continue;
            }

            var normalSavePath = Directory.EnumerateFiles(profileDirectory)
                .FirstOrDefault(path => Path.GetFileName(path).Equals(
                    NormalSaveName,
                    StringComparison.OrdinalIgnoreCase));
            if (normalSavePath is not null)
            {
                candidates.Add(new SaveProfileCandidate(steamId, Path.GetFullPath(normalSavePath)));
            }
        }

        var sortedCandidates = candidates
            .OrderBy(candidate => candidate.SteamId, StringComparer.Ordinal)
            .ToArray();

        cancellationToken.ThrowIfCancellationRequested();
        return Task.FromResult<IReadOnlyList<SaveProfileCandidate>>(sortedCandidates);
    }

    [GeneratedRegex("^[0-9]{1,20}$", RegexOptions.CultureInvariant)]
    private static partial Regex SteamIdPattern();
}
