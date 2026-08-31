using System.Text;
using System.Text.Json;

namespace DSRRandomizer.Launcher.Safety;

internal static class ReleaseArtifactIdentityValidator
{
    private const string GuardRelativePath = "native/DSRRandomizer.Runtime.dll";
    private const string ProfileRelativePath = "config/compatibility-profiles.json";
    private const string SidecarRelativePath = "native/DSRRandomizer.Runtime.dll.sha256";
    private const string BridgeRelativePath = "components/rmm-bridge/DSRRandomizer.RmmBridge.dll";
    private const string HostRelativePath = "components/rmm-bridge/DSRRandomizer.RmmBridgeHost.exe";
    private const string ManifestRelativePath = "components/rmm-bridge/deployment-manifest.json";

    public static IReadOnlyList<string> Validate(string packageRoot)
    {
        var identities = LaunchArtifactIdentities.LoadEmbedded();
        using var guard = LaunchArtifactLease.TryOpen(Resolve(packageRoot, GuardRelativePath));
        using var profile = LaunchArtifactLease.TryOpen(Resolve(packageRoot, ProfileRelativePath));
        using var sidecar = LaunchArtifactLease.TryOpen(Resolve(packageRoot, SidecarRelativePath));
        using var bridge = LaunchArtifactLease.TryOpen(Resolve(packageRoot, BridgeRelativePath));
        using var host = LaunchArtifactLease.TryOpen(Resolve(packageRoot, HostRelativePath));
        using var manifest = LaunchArtifactLease.TryOpen(Resolve(packageRoot, ManifestRelativePath));
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

        if (bridge is null)
        {
            failures.Add($"invalid:{BridgeRelativePath}");
        }
        else if (!bridge.Sha256.Equals(identities.BridgeSha256, StringComparison.Ordinal))
        {
            failures.Add($"mismatch:{BridgeRelativePath}");
        }

        if (host is null)
        {
            failures.Add($"invalid:{HostRelativePath}");
        }
        else if (!host.Sha256.Equals(identities.HostSha256, StringComparison.Ordinal))
        {
            failures.Add($"mismatch:{HostRelativePath}");
        }

        if (manifest is null)
        {
            failures.Add($"invalid:{ManifestRelativePath}");
        }
        else if (!MatchesManifest(manifest.Bytes, identities))
        {
            failures.Add($"mismatch:{ManifestRelativePath}");
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

    private static bool MatchesManifest(byte[] bytes, LaunchArtifactIdentities identities)
    {
        try
        {
            using var document = JsonDocument.Parse(bytes);
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object || root.GetRawText().Length == 0)
            {
                return false;
            }

            var properties = root.EnumerateObject().ToArray();
            if (properties.Length != 5
                || properties.Any(property => property.Name is not (
                    "schemaVersion" or "configuration" or "runtimeId" or "bridgeSha256" or "hostSha256")))
            {
                return false;
            }

            return root.TryGetProperty("schemaVersion", out var schemaVersion)
                   && schemaVersion.ValueKind == JsonValueKind.Number
                   && schemaVersion.TryGetInt32(out var schema)
                   && schema == 1
                   && root.TryGetProperty("configuration", out var configuration)
                   && configuration.ValueKind == JsonValueKind.String
                   && configuration.GetString() == "Release"
                   && root.TryGetProperty("runtimeId", out var runtimeId)
                   && runtimeId.ValueKind == JsonValueKind.String
                   && !string.IsNullOrEmpty(runtimeId.GetString())
                   && root.TryGetProperty("bridgeSha256", out var bridgeHash)
                   && bridgeHash.ValueKind == JsonValueKind.String
                   && bridgeHash.GetString() == identities.BridgeSha256
                   && root.TryGetProperty("hostSha256", out var hostHash)
                   && hostHash.ValueKind == JsonValueKind.String
                   && hostHash.GetString() == identities.HostSha256;
        }
        catch (JsonException)
        {
            return false;
        }
    }

    private static string Resolve(string root, string relativePath) => Path.Combine(
        root,
        relativePath.Replace('/', Path.DirectorySeparatorChar));
}
