using System.Text;

namespace DSRRandomizer.Launcher.Safety;

internal static class ReleaseArtifactIdentityValidator
{
    private const string GuardRelativePath = "native/DSRRandomizer.Runtime.dll";
    private const string ProfileRelativePath = "config/compatibility-profiles.json";
    private const string SidecarRelativePath = "native/DSRRandomizer.Runtime.dll.sha256";

    public static IReadOnlyList<string> Validate(string packageRoot)
    {
        var identities = LaunchArtifactIdentities.LoadEmbedded();
        using var guard = LaunchArtifactLease.TryOpen(Resolve(packageRoot, GuardRelativePath));
        using var profile = LaunchArtifactLease.TryOpen(Resolve(packageRoot, ProfileRelativePath));
        using var sidecar = LaunchArtifactLease.TryOpen(Resolve(packageRoot, SidecarRelativePath));
        var failures = new List<string>();

        if (guard is null)
        {
            failures.Add($"invalid:{GuardRelativePath}");
        }
        else if (!guard.Sha256.Equals(identities.GuardSha256, StringComparison.Ordinal))
        {
            failures.Add($"mismatch:{GuardRelativePath}");
        }

        if (profile is null)
        {
            failures.Add($"invalid:{ProfileRelativePath}");
        }
        else if (!profile.Sha256.Equals(identities.ProfileSha256, StringComparison.Ordinal))
        {
            failures.Add($"mismatch:{ProfileRelativePath}");
        }

        if (sidecar is null)
        {
            failures.Add($"invalid:{SidecarRelativePath}");
        }
        else if (guard is not null && !MatchesGuard(sidecar.Bytes, guard.Sha256))
        {
            failures.Add($"mismatch:{SidecarRelativePath}");
        }

        return failures;
    }

    private static bool MatchesGuard(byte[] bytes, string guardSha256)
    {
        string value;
        try
        {
            value = new UTF8Encoding(encoderShouldEmitUTF8Identifier: false, throwOnInvalidBytes: true)
                .GetString(bytes)
                .TrimEnd('\r', '\n');
        }
        catch (DecoderFallbackException)
        {
            return false;
        }

        return value.Length == 64
            && value.All(Uri.IsHexDigit)
            && value.Equals(guardSha256, StringComparison.OrdinalIgnoreCase);
    }

    private static string Resolve(string root, string relativePath) => Path.Combine(
        root,
        relativePath.Replace('/', Path.DirectorySeparatorChar));
}
