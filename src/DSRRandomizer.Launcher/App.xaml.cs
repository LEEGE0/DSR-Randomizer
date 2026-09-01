using System.Windows;
using DSRRandomizer.Foundation.Installation;
using DSRRandomizer.Foundation.Paths;
using DSRRandomizer.Launcher.Configuration;
using DSRRandomizer.Launcher.Logging;
using DSRRandomizer.Launcher.Services;
using DSRRandomizer.Launcher.ViewModels;

namespace DSRRandomizer.Launcher;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        var localRoot = Program.GetLocalDataRoot();
        var rootStore = new ExternalRootSelectionStore(localRoot);
        string? externalRoot;
        try
        {
            externalRoot = rootStore.ReadAsync(CancellationToken.None).GetAwaiter().GetResult();
        }
        catch (Exception exception) when (
            exception is IOException
                or UnauthorizedAccessException
                or ArgumentException)
        {
            externalRoot = null;
        }

        if (externalRoot is null)
        {
            MainWindow = new MainWindow(new MainWindowViewModel(
                service: null,
                new NoOpExternalLogger(),
                materialRoot: string.Empty,
                rootStore,
                materialOperationsAvailable: false));
            MainWindow.Show();
            return;
        }

        var canonicalizer = new WindowsPathCanonicalizer();
        var selectedSource = InstallationSelectionStore
            .CreateReadOnly(externalRoot, canonicalizer)
            .ReadAsync(CancellationToken.None)
            .GetAwaiter()
            .GetResult();
        var protectedSource = selectedSource ?? Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
            "DSR-Randomizer-Protected-Source");
        var boundary = WriteBoundary.Create(protectedSource, externalRoot, canonicalizer);
        var layout = LocalDataLayout.Create(externalRoot, boundary);
        var viewModel = new MainWindowViewModel(
            new LauncherService(externalRoot),
            new FileExternalLogger(layout, boundary),
            externalRoot,
            rootStore)
        {
            GamePath = selectedSource ?? string.Empty
        };
        MainWindow = new MainWindow(viewModel);
        MainWindow.Show();
    }

    private sealed class NoOpExternalLogger : IExternalLogger
    {
        public Task LogExceptionAsync(Exception exception, CancellationToken cancellationToken) => Task.CompletedTask;
    }
}
