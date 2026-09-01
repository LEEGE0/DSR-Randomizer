using DSRRandomizer.Launcher.Logging;
using DSRRandomizer.Launcher.ViewModels;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

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

    [Fact]
    public void Window_ContainsItemAndEnemyRandomizerButtons()
    {
        Exception? failure = null;
        var foundContents = new List<string>();
        var thread = new Thread(() =>
        {
            try
            {
                var window = new MainWindow(CreateViewModel());
                window.Show();
                foundContents.AddRange(FindVisualChildren<Button>(window)
                    .Select(button => button.Content as string)
                    .Where(content => content is not null)!);
                window.Close();
            }
            catch (Exception exception)
            {
                failure = exception;
            }
        });
        thread.SetApartmentState(ApartmentState.STA);

        thread.Start();

        Assert.True(thread.Join(TimeSpan.FromSeconds(10)), "The WPF window did not finish opening.");
        Assert.Null(failure);
        Assert.Contains("Launch Item Randomizer", foundContents);
        Assert.Contains("Launch Enemy Randomizer", foundContents);
    }

    private static IEnumerable<T> FindVisualChildren<T>(DependencyObject root)
        where T : DependencyObject
    {
        if (root is T match)
        {
            yield return match;
        }

        for (var index = 0; index < VisualTreeHelper.GetChildrenCount(root); index++)
        {
            foreach (var child in FindVisualChildren<T>(VisualTreeHelper.GetChild(root, index)))
            {
                yield return child;
            }
        }
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
