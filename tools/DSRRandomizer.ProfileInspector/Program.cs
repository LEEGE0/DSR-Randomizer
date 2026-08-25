using DSRRandomizer.Foundation.Safety;

if (args.Length == 2 && string.Equals(args[0], "validate", StringComparison.Ordinal))
{
    try
    {
        _ = CompatibilityProfileCatalog.Load(args[1]);
        return 0;
    }
    catch (Exception exception) when (
        exception is IOException
            or UnauthorizedAccessException
            or CompatibilityProfileFormatException)
    {
        Console.Error.WriteLine($"Profile validation failed: {exception.Message}");
        return 1;
    }
}

if (args.Length != 3 || !string.Equals(args[0], "verify", StringComparison.Ordinal))
{
    Console.Error.WriteLine(
        "Usage: DSRRandomizer.ProfileInspector validate <compatibility-profiles.json>\n" +
        "   or: DSRRandomizer.ProfileInspector verify <executable> <compatibility-profiles.json>");
    return 2;
}

try
{
    var identity = ProfileInspector.InspectIdentity(args[1]);
    var profile = CompatibilityProfileCatalog.Load(args[2]).Select(identity);
    var result = ProfileInspector.VerifyFiles(args[1], profile);
    if (result.Error != ProfileError.None)
    {
        Console.Error.WriteLine($"Profile verification failed: {result.Error}.");
        return 1;
    }

    Console.WriteLine(
        $"Verified profile {profile.Id}: length={identity.Length}, " +
        $"sha256={identity.Sha256}, machine=0x{identity.Machine:x4}, " +
        $"timestamp=0x{identity.PeTimestamp:x8}, imageSize=0x{identity.SizeOfImage:x}, " +
        $"targets={profile.GameServiceTargets.Count}.");
    return 0;
}
catch (Exception exception) when (
    exception is IOException
        or UnauthorizedAccessException
        or BadImageFormatException
        or CompatibilityProfileFormatException
        or UnsupportedGameBuildException)
{
    Console.Error.WriteLine($"Profile verification failed: {exception.Message}");
    return 1;
}
