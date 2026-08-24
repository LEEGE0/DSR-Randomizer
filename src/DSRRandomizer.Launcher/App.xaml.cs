using System.Windows;
using DSRRandomizer.Foundation.Installation;
using DSRRandomizer.Foundation.Paths;
using DSRRandomizer.Launcher.Logging;
using DSRRandomizer.Launcher.Services;
using DSRRandomizer.Launcher.ViewModels;

namespace DSRRandomizer.Launcher;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        var localRoot = GetLocalDataRoot();
        var service = new LauncherService(localRoot);

        var canonicalizer = new WindowsPathCanonicalizer();
        var selectedSource = InstallationSelectionStore
            .CreateReadOnly(localRoot, canonicalizer)
            .ReadAsync(CancellationToken.None)
            .GetAwaiter()
            .GetResult();
        var protectedSource = selectedSource ?? Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
            "DSR-Randomizer-Protected-Source");
        var boundary = WriteBoundary.Create(protectedSource, localRoot, canonicalizer);
        var layout = LocalDataLayout.Create(localRoot, boundary);
        var viewModel = new MainWindowViewModel(
            service,
            new FileExternalLogger(layout, boundary))
        {
            GamePath = selectedSource ?? string.Empty
        };
        MainWindow = new MainWindow(viewModel);
        MainWindow.Show();
    }

    private static string GetLocalDataRoot()
    {
        var localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        if (string.IsNullOrWhiteSpace(localAppData))
        {
            throw new InvalidOperationException("The Windows local application-data path is unavailable.");
        }

        return Path.Combine(localAppData, "DSR-Randomizer");
    }
}
