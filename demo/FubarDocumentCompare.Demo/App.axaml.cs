using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using FubarDocumentCompare.Demo.ViewModels;
using FubarDocumentCompare.Demo.Views;

namespace FubarDocumentCompare.Demo;

public partial class App : Application
{
    public override void Initialize() => AvaloniaXamlLoader.Load(this);

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            var engine = new DocumentCompareEngine(new EngineOptions { MaxParallelJobs = 2 });
            desktop.MainWindow = new MainWindow { DataContext = new MainViewModel(engine) };
            desktop.Exit += (_, _) => engine.Dispose();
        }
        base.OnFrameworkInitializationCompleted();
    }
}
