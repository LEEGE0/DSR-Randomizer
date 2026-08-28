using DSRRandomizer.Launcher.Logging;
using DSRRandomizer.Launcher.ViewModels;

namespace DSRRandomizer.Launcher.Tests;

public sealed class MainWindowTests
{
    [Fact]
    public void Assembly_UsesDsrForModLauncherName()
    {
        Assert.Equal("DSRForMod.Launcher", typeof(MainWindow).Assembly.GetName().Name);
    }

    [Fact]
    public void Window_UsesDsrForModDisplayName()
    {
        Exception? failure = null;
        string? title = null;
        var thread = new Thread(() =>
        {
            try
            {
                var window = new MainWindow(CreateViewModel());
                title = window.Title;
                window.Close();
            }
            catch (Exception exception)
            {
                failure = exception;
            }
        });
        thread.SetApartmentState(ApartmentState.STA);

        thread.Start();

        Assert.True(thread.Join(TimeSpan.FromSeconds(10)), "The WPF window did not finish loading.");
        Assert.Null(failure);
        Assert.Equal("DSR for MOD — Manual Offline Mod Runtime", title);
    }

    [Fact]
    public void Show_WithReadOnlyProgressProperty_DoesNotCrash()
    {
        Exception? failure = null;
        var completed = false;
        var thread = new Thread(() =>
        {
            try
            {
                var window = new MainWindow(CreateViewModel());

                window.Show();
                window.Close();
                completed = true;
            }
            catch (Exception exception)
            {
                failure = exception;
            }
        });
        thread.SetApartmentState(ApartmentState.STA);

        thread.Start();

        Assert.True(thread.Join(TimeSpan.FromSeconds(10)), "The WPF window did not finish opening.");
        Assert.True(completed);
        Assert.Null(failure);
    }

    private static MainWindowViewModel CreateViewModel() => new(
        service: null,
        new NoOpLogger(),
        materialRoot: string.Empty,
        materialOperationsAvailable: false);

    private sealed class NoOpLogger : IExternalLogger
    {
        public Task LogExceptionAsync(Exception exception, CancellationToken cancellationToken) =>
            Task.CompletedTask;
    }
}
