using DSRRandomizer.Foundation.Paths;
using DSRRandomizer.Foundation.Saves;

namespace DSRRandomizer.Foundation.Tests.Saves;

public sealed class SaveContractsTests
{
    [Fact]
    public void DedicatedPath_IsNamespacedByNumericSteamId()
    {
        var path = SavePaths.GetDedicatedSave(
            @"C:\Local\DSR",
            "12345678901234567",
            CreateBoundary());

        Assert.Equal(@"C:\Local\DSR\saves\12345678901234567\DRAKS0005.rmm", path);
        Assert.Throws<ArgumentException>(
            () => SavePaths.GetDedicatedSave(@"C:\Local\DSR", "../escape", CreateBoundary()));
    }

    [Fact]
    public void DedicatedPath_AcceptsNineDigitNumericSaveFolder()
    {
        var path = SavePaths.GetDedicatedSave(
            @"C:\Local\DSR",
            "123456789",
            CreateBoundary());

        Assert.Equal(@"C:\Local\DSR\saves\123456789\DRAKS0005.rmm", path);
    }

    [Fact]
    public void DedicatedPath_PublicApiRequiresWriteBoundary()
    {
        var uncheckedResolver = typeof(SavePaths).GetMethod(
            nameof(SavePaths.GetDedicatedSave),
            [typeof(string), typeof(string)]);

        Assert.Null(uncheckedResolver);
    }

    [Fact]
    public void DedicatedPath_IsDeniedWhenTheBoundaryRejectsItsCanonicalDestination()
    {
        var canonicalizer = new FakeCanonicalizer(new Dictionary<string, string>
        {
            [@"C:\Steam\DSR"] = @"C:\Steam\DSR",
            [@"C:\Local\DSR"] = @"C:\Local\DSR",
            [@"C:\Local\DSR\saves\12345678901234567\DRAKS0005.rmm"] = @"C:\Steam\DSR\DRAKS0005.rmm"
        });
        var boundary = WriteBoundary.Create(@"C:\Steam\DSR", @"C:\Local\DSR", canonicalizer);

        Assert.Throws<UnauthorizedAccessException>(
            () => SavePaths.GetDedicatedSave(@"C:\Local\DSR", "12345678901234567", boundary));
    }

    [Fact]
    public void DedicatedSaveContracts_PreserveTheirSpecifiedValues()
    {
        var candidate = new SaveProfileCandidate("12345678901234567", @"C:\Documents\12345678901234567\DRAKS0005.sl2");
        var metadata = new DedicatedSaveMetadata(1, candidate.SteamId, 4_326_608, "save-sha", "seed-1", "placement-sha", true);
        var binding = new SeedBinding("seed-1", "placement-sha");

        var failure = DedicatedSaveResult.Fail(SaveErrorCode.SeedMismatch, "seed binding changed");

        Assert.Equal("12345678901234567", candidate.SteamId);
        Assert.Equal(4_326_608, metadata.FixedLength);
        Assert.Equal("placement-sha", binding.PlacementSha256);
        Assert.Equal(new DedicatedSaveResult(false, false, null, SaveErrorCode.SeedMismatch, "seed binding changed"), failure);
        Assert.Equal(
            new[]
            {
                SaveErrorCode.None,
                SaveErrorCode.InvalidSteamId,
                SaveErrorCode.SourceMissing,
                SaveErrorCode.MultipleProfilesRequireSelection,
                SaveErrorCode.ExistingSaveInvalid,
                SaveErrorCode.CopyVerificationFailed,
                SaveErrorCode.SourceChanged,
                SaveErrorCode.DestinationRace,
                SaveErrorCode.SeedMismatch,
                SaveErrorCode.PathDenied,
                SaveErrorCode.FirstCopyConfirmationRequired,
                SaveErrorCode.RecoveryRequired,
                SaveErrorCode.SessionAlreadyActive
            },
            Enum.GetValues<SaveErrorCode>());
        Assert.Equal(3, (int)SaveErrorCode.MultipleProfilesRequireSelection);
        Assert.Equal(9, (int)SaveErrorCode.PathDenied);
        Assert.Equal(10, (int)SaveErrorCode.FirstCopyConfirmationRequired);
        Assert.Equal(11, (int)SaveErrorCode.RecoveryRequired);
        Assert.Equal(12, (int)SaveErrorCode.SessionAlreadyActive);
    }

    private sealed class FakeCanonicalizer(IReadOnlyDictionary<string, string> mappings) : IPathCanonicalizer
    {
        public string Canonicalize(string path) => mappings.TryGetValue(path, out var mapped)
            ? mapped
            : Path.GetFullPath(path).TrimEnd(Path.DirectorySeparatorChar);
    }

    private static WriteBoundary CreateBoundary() => WriteBoundary.Create(
        @"C:\Steam\DSR",
        @"C:\Local\DSR",
        new FakeCanonicalizer(new Dictionary<string, string>()));
}
