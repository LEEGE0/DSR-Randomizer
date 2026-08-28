using System.Text.Json;
using DSRRandomizer.Foundation.Installation;
using DSRRandomizer.Foundation.Paths;
using DSRRandomizer.Launcher.Configuration;
using DSRRandomizer.Launcher.Services;

namespace DSRRandomizer.Launcher;

public static class Program
{
    [STAThread]
    public static int Main(string[] args)
    {
        return RunWithLocalDataRoot(args, GetLocalDataRoot(), Console.Out, Console.Error);
    }

    internal static int RunWithLocalDataRoot(
        string[] args,
        string localRoot,
        TextWriter output,
        TextWriter error)
    {
        ArgumentNullException.ThrowIfNull(args);
        ArgumentException.ThrowIfNullOrWhiteSpace(localRoot);
        ArgumentNullException.ThrowIfNull(output);
        ArgumentNullException.ThrowIfNull(error);
        var rootStore = new ExternalRootSelectionStore(localRoot);
        if (args is ["--set-root", var externalRoot])
        {
            try
            {
                var selectedSource = TryReadPreviousSource(rootStore);
                rootStore.WriteAsync(externalRoot, selectedSource, CancellationToken.None).GetAwaiter().GetResult();
                var selectedRoot = rootStore.ReadAsync(CancellationToken.None).GetAwaiter().GetResult();
                output.WriteLine(JsonSerializer.Serialize(new { success = true, root = selectedRoot }));
                return 0;
            }
            catch (Exception exception) when (
                exception is IOException
                    or UnauthorizedAccessException
                    or ArgumentException
                    or JsonException)
            {
                error.WriteLine(exception.Message);
                output.WriteLine(JsonSerializer.Serialize(new
                {
                    success = false,
                    error = exception.Message
                }));
                return 2;
            }
        }

        if (args.Length > 0)
        {
            string? selectedRoot;
            try
            {
                selectedRoot = rootStore.ReadAsync(CancellationToken.None).GetAwaiter().GetResult();
            }
            catch (Exception exception) when (
                exception is IOException
                    or UnauthorizedAccessException
                    or ArgumentException
                    or JsonException)
            {
                error.WriteLine(exception.Message);
                selectedRoot = null;
            }

            return new LauncherApplication(
                    selectedRoot is null ? null : new LauncherService(selectedRoot),
                    output,
                    error,
                    externalRootSelected: selectedRoot is not null)
                .RunAsync(args, CancellationToken.None)
                .GetAwaiter()
                .GetResult();
        }

        return new App().Run();
    }

    private static string? TryReadPreviousSource(ExternalRootSelectionStore rootStore)
    {
        try
        {
            var existingRoot = rootStore.ReadAsync(CancellationToken.None).GetAwaiter().GetResult();
            return existingRoot is null
                ? null
                : InstallationSelectionStore.CreateReadOnly(
                        existingRoot,
                        new WindowsPathCanonicalizer())
                    .ReadAsync(CancellationToken.None)
                    .GetAwaiter()
                    .GetResult();
        }
        catch (Exception exception) when (
            exception is IOException
                or UnauthorizedAccessException
                or ArgumentException
                or JsonException)
        {
            return null;
        }
    }

    internal static string GetLocalDataRoot()
    {
        var localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        if (string.IsNullOrWhiteSpace(localAppData))
        {
            throw new InvalidOperationException("The Windows local application-data path is unavailable.");
        }

        return Path.Combine(localAppData, "DSR-Randomizer");
    }
}
