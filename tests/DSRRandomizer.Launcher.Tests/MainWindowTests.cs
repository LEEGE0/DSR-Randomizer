using DSRRandomizer.Launcher.Logging;
using DSRRandomizer.Launcher.ViewModels;

namespace DSRRandomizer.Launcher.Tests;

public sealed class MainWindowTests
{
    [Fact]
    public void Show_WithReadOnlyProgressProperty_DoesNotCrash()
    {
        Exception? failure = null;
        var completed = false;
        var thread = new Thread(() =>
        {
            try
            {
                var viewModel = new MainWindowViewModel(
                    service: null,
                    new NoOpLogger(),
                    materialRoot: string.Empty,
                    materialOperationsAvailable: false);
                var window = new MainWindow(viewModel);

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

    private sealed class NoOpLogger : IExternalLogger
    {
        public Task LogExceptionAsync(Exception exception, CancellationToken cancellationToken) =>
            Task.CompletedTask;
    }
}
