using DSRRandomizer.Launcher.Services;

namespace DSRRandomizer.Launcher;

public static class Program
{
    [STAThread]
    public static int Main(string[] args)
    {
        if (args.Length > 0)
        {
            return new LauncherApplication(
                    LauncherService.CreateDefault(),
                    Console.Out,
                    Console.Error)
                .RunAsync(args, CancellationToken.None)
                .GetAwaiter()
                .GetResult();
        }

        return new App().Run();
    }
}
