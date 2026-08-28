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
        var localRoot = GetLocalDataRoot();
        var rootStore = new ExternalRootSelectionStore(localRoot);
        if (args is ["--set-root", var externalRoot])
        {
            try
            {
                var existingRoot = rootStore.ReadAsync(CancellationToken.None).GetAwaiter().GetResult();
                var selectedSource = existingRoot is null
                    ? null
                    : InstallationSelectionStore.CreateReadOnly(
                            existingRoot,
                            new WindowsPathCanonicalizer())
                        .ReadAsync(CancellationToken.None)
                        .GetAwaiter()
                        .GetResult();
                rootStore.WriteAsync(externalRoot, selectedSource, CancellationToken.None).GetAwaiter().GetResult();
                var selectedRoot = rootStore.ReadAsync(CancellationToken.None).GetAwaiter().GetResult();
                Console.Out.WriteLine(JsonSerializer.Serialize(new { success = true, root = selectedRoot }));
                return 0;
            }
            catch (Exception exception) when (
                exception is IOException
                    or UnauthorizedAccessException
                    or ArgumentException
                    or JsonException)
            {
                Console.Error.WriteLine(exception.Message);
                Console.Out.WriteLine(JsonSerializer.Serialize(new
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
                Console.Error.WriteLine(exception.Message);
                selectedRoot = null;
            }

            return new LauncherApplication(
                    selectedRoot is null ? null : new LauncherService(selectedRoot),
                    Console.Out,
                    Console.Error,
                    externalRootSelected: selectedRoot is not null)
                .RunAsync(args, CancellationToken.None)
                .GetAwaiter()
                .GetResult();
        }

        return new App().Run();
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
