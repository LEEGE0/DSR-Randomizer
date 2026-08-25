using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text;

namespace DSRRandomizer.Foundation.Safety;

public sealed class CompatibilityProfileCatalog
{
    private const int SupportedSchemaVersion = 1;
    private const string ProfileFileName = "compatibility-profiles.json";
    private static readonly Lazy<CompatibilityProfileCatalog> DefaultCatalog =
        new(() => Load(ResolveDefaultPath(AppContext.BaseDirectory)));
    private static readonly JsonSerializerOptions JsonOptions = CreateJsonOptions();

    private readonly IReadOnlyDictionary<ExecutableKey, CompatibilityProfile> _profiles;

    public CompatibilityProfileCatalog(IEnumerable<CompatibilityProfile> profiles)
    {
        ArgumentNullException.ThrowIfNull(profiles);

        var byIdentity = new Dictionary<ExecutableKey, CompatibilityProfile>();
        foreach (var profile in profiles)
        {
            ArgumentNullException.ThrowIfNull(profile);
            var key = CreateKey(profile.Executable);
            if (!byIdentity.TryAdd(key, profile))
            {
                throw new ArgumentException(
                    "Compatibility profiles contain a duplicate executable identity.",
                    nameof(profiles));
            }
        }

        _profiles = byIdentity;
    }

    public static CompatibilityProfileCatalog Default => DefaultCatalog.Value;

    public static CompatibilityProfileCatalog Load(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        try
        {
            return LoadJson(File.ReadAllText(path));
        }
        catch (CompatibilityProfileFormatException)
        {
            throw;
        }
        catch (Exception exception) when (
            exception is IOException or UnauthorizedAccessException)
        {
            throw new CompatibilityProfileFormatException(
                "The release-pinned compatibility profile could not be read.",
                exception);
        }
    }

    public static CompatibilityProfileCatalog LoadJson(string json)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(json);
        try
        {
            ValidateNoDuplicateProperties(json);
            var document = JsonSerializer.Deserialize<ProfileDocument>(json, JsonOptions)
                ?? throw new CompatibilityProfileFormatException(
                    "The compatibility profile document is empty.");
            if (document.SchemaVersion != SupportedSchemaVersion)
            {
                throw new CompatibilityProfileFormatException(
                    $"Compatibility profile schema {document.SchemaVersion} is unsupported.");
            }
            if (document.Profiles is not { Count: > 0 })
            {
                throw new CompatibilityProfileFormatException(
                    "At least one compatibility profile is required.");
            }

            var ids = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            var profiles = new List<CompatibilityProfile>(document.Profiles.Count);
            foreach (var source in document.Profiles)
            {
                var profile = ConvertProfile(source);
                if (!ids.Add(profile.Id))
                {
                    throw new CompatibilityProfileFormatException(
                        "Compatibility profile IDs must be unique.");
                }
                profiles.Add(profile);
            }

            try
            {
                return new CompatibilityProfileCatalog(profiles);
            }
            catch (ArgumentException exception)
            {
                throw new CompatibilityProfileFormatException(
                    "Compatibility profiles contain a duplicate executable identity.",
                    exception);
            }
        }
        catch (CompatibilityProfileFormatException)
        {
            throw;
        }
        catch (JsonException exception)
        {
            throw new CompatibilityProfileFormatException(
                "The compatibility profile JSON is malformed or does not match schema v1.",
                exception);
        }
    }

    public CompatibilityProfile Select(ExecutableIdentity executable)
    {
        ArgumentNullException.ThrowIfNull(executable);
        var key = CreateKey(executable);
        if (_profiles.TryGetValue(key, out var profile))
        {
            return profile;
        }

        throw new UnsupportedGameBuildException(
            $"The Dark Souls Remastered build is unsupported: {key.Sha256}.");
    }

    private static CompatibilityProfile ConvertProfile(ProfileDto source)
    {
        if (string.IsNullOrWhiteSpace(source.Id) ||
            !IsSafeModuleName(source.ExecutableModule) ||
            source.FixedSaveLength <= 0 || source.ProtocolVersion != 2 ||
            source.Executable is null)
        {
            throw new CompatibilityProfileFormatException(
                "A compatibility profile contains invalid required fields.");
        }

        var modules = (source.Modules ?? [])
            .Select(ConvertModule)
            .ToArray();
        var moduleNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
        {
            source.ExecutableModule
        };
        if (modules.Any(module => !moduleNames.Add(module.Name)))
        {
            throw new CompatibilityProfileFormatException(
                "Module names must be unique within a compatibility profile.");
        }
        if (modules.GroupBy(module => CreateKey(module.Identity))
            .Any(group => group.Count() != 1))
        {
            throw new CompatibilityProfileFormatException(
                "Module identities must be unique within a compatibility profile.");
        }

        var targets = (source.GameServiceTargets ?? [])
            .Select(ConvertTarget)
            .ToArray();
        if (targets.Length == 0 ||
            !targets.Any(target => target.Action == InternalTargetAction.ForceOffline) ||
            !targets.Any(target => target.Action == InternalTargetAction.DenyCall))
        {
            throw new CompatibilityProfileFormatException(
                "Game-service targets must include ForceOffline and DenyCall actions.");
        }
        if (targets.Any(target => !moduleNames.Contains(target.Module)))
        {
            throw new CompatibilityProfileFormatException(
                "Every game-service target must name a declared module.");
        }
        if (targets.GroupBy(
                target => (target.Module.ToUpperInvariant(), target.Rva))
            .Any(group => group.Count() != 1))
        {
            throw new CompatibilityProfileFormatException(
                "Game-service target module/RVA pairs must be unique.");
        }

        return new CompatibilityProfile(
            source.Id,
            source.ExecutableModule,
            ConvertIdentity(source.Executable),
            source.FixedSaveLength,
            source.ProtocolVersion,
            modules,
            targets);
    }

    private static ModuleProfile ConvertModule(ModuleDto source)
    {
        if (!IsSafeModuleName(source.Name) ||
            source.AllowDeferred is null ||
            !ValidUniqueNames(source.DeclaredInterfaces) ||
            !ValidUniqueNames(source.ProtectedFactoryExports))
        {
            throw new CompatibilityProfileFormatException(
                "A compatibility module contains invalid gate declarations.");
        }
        return new ModuleProfile(
            source.Name,
            ConvertIdentity(source),
            source.AllowDeferred.Value,
            source.DeclaredInterfaces!.ToArray(),
            source.ProtectedFactoryExports!.ToArray());
    }

    private static bool ValidUniqueNames(List<string>? names) =>
        names is { Count: > 0 } &&
        names.All(name => !string.IsNullOrWhiteSpace(name) &&
            name.All(character => char.IsAsciiLetterOrDigit(character) || character == '_')) &&
        names.Distinct(StringComparer.Ordinal).Count() == names.Count;

    private static bool IsSafeModuleName(string? name) =>
        !string.IsNullOrWhiteSpace(name) &&
        Path.GetFileName(name) == name &&
        name.All(character => char.IsAsciiLetterOrDigit(character)
            || character is '.' or '_' or '-');

    private static InternalTargetProfile ConvertTarget(TargetDto source)
    {
        if (string.IsNullOrWhiteSpace(source.Module) || source.Rva == 0 ||
            source.PatchLength <= 0 || !IsSha256(source.FingerprintSha256) ||
            !Enum.IsDefined(source.Action))
        {
            throw new CompatibilityProfileFormatException(
                "A game-service target contains invalid required fields.");
        }
        return new InternalTargetProfile(
            source.Module,
            source.Rva,
            source.FingerprintSha256.ToLowerInvariant(),
            source.PatchLength,
            source.Action);
    }

    private static ExecutableIdentity ConvertIdentity(IdentityDto source)
    {
        if (source.Length <= 0 || source.Machine == 0 || source.PeTimestamp == 0 ||
            source.SizeOfImage == 0 || !IsSha256(source.Sha256))
        {
            throw new CompatibilityProfileFormatException(
                "A module identity contains invalid required fields.");
        }
        return new ExecutableIdentity(
            source.Length,
            source.Sha256.ToLowerInvariant(),
            source.Machine,
            source.PeTimestamp,
            source.SizeOfImage);
    }

    private static ExecutableKey CreateKey(ExecutableIdentity executable)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(executable.Sha256);
        if (!IsSha256(executable.Sha256))
        {
            throw new ArgumentException(
                "Executable SHA-256 must contain exactly 64 hexadecimal characters.",
                nameof(executable));
        }

        return new ExecutableKey(
            executable.Length,
            executable.Sha256.ToLowerInvariant(),
            executable.Machine,
            executable.PeTimestamp,
            executable.SizeOfImage);
    }

    private static bool IsSha256(string? value) =>
        value is { Length: 64 } && value.All(Uri.IsHexDigit);

    internal static string ResolveDefaultPath(string applicationBaseDirectory)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(applicationBaseDirectory);
        var root = Path.GetFullPath(applicationBaseDirectory);
        var candidate = Path.Combine(root, "config", ProfileFileName);
        if (File.Exists(candidate))
        {
            return candidate;
        }
        throw new CompatibilityProfileFormatException(
            "The release-pinned compatibility profile document was not found.");
    }

    private static void ValidateNoDuplicateProperties(string json)
    {
        var reader = new Utf8JsonReader(
            Encoding.UTF8.GetBytes(json),
            new JsonReaderOptions
            {
                AllowTrailingCommas = false,
                CommentHandling = JsonCommentHandling.Disallow
            });
        var objects = new Stack<HashSet<string>>();
        while (reader.Read())
        {
            if (reader.TokenType == JsonTokenType.StartObject)
            {
                objects.Push(new HashSet<string>(StringComparer.Ordinal));
            }
            else if (reader.TokenType == JsonTokenType.EndObject)
            {
                _ = objects.Pop();
            }
            else if (reader.TokenType == JsonTokenType.PropertyName)
            {
                var name = reader.GetString() ?? throw new JsonException(
                    "A JSON property name is invalid.");
                if (objects.Count == 0 || !objects.Peek().Add(name))
                {
                    throw new JsonException(
                        $"The JSON object contains duplicate property '{name}'.");
                }
            }
        }
    }

    private static JsonSerializerOptions CreateJsonOptions()
    {
        var options = new JsonSerializerOptions
        {
            PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
            PropertyNameCaseInsensitive = false,
            UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow
        };
        options.Converters.Add(
            new JsonStringEnumConverter<InternalTargetAction>(
                namingPolicy: null,
                allowIntegerValues: false));
        return options;
    }

    private sealed record ProfileDocument(
        int SchemaVersion,
        List<ProfileDto>? Profiles);

    private sealed record ProfileDto(
        string Id,
        string ExecutableModule,
        IdentityDto? Executable,
        long FixedSaveLength,
        ushort ProtocolVersion,
        List<ModuleDto>? Modules,
        List<TargetDto>? GameServiceTargets);

    private record IdentityDto(
        long Length,
        string Sha256,
        ushort Machine,
        uint PeTimestamp,
        uint SizeOfImage);

    private sealed record ModuleDto(
        string Name,
        long Length,
        string Sha256,
        ushort Machine,
        uint PeTimestamp,
        uint SizeOfImage,
        bool? AllowDeferred,
        List<string>? DeclaredInterfaces,
        List<string>? ProtectedFactoryExports)
        : IdentityDto(Length, Sha256, Machine, PeTimestamp, SizeOfImage);

    private sealed record TargetDto(
        string Module,
        uint Rva,
        string FingerprintSha256,
        int PatchLength,
        InternalTargetAction Action);

    private sealed record ExecutableKey(
        long Length,
        string Sha256,
        ushort Machine,
        uint PeTimestamp,
        uint SizeOfImage);
}
