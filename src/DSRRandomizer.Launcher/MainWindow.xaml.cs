using System.Windows;
using DSRRandomizer.Launcher.ViewModels;

namespace DSRRandomizer.Launcher;

public partial class MainWindow : Window
{
    public MainWindow(MainWindowViewModel viewModel)
    {
        InitializeComponent();
        DataContext = viewModel;
    }
}
