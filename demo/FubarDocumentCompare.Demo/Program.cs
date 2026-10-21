using Avalonia;
using Avalonia.Headless;
using Avalonia.Threading;
using Avalonia.VisualTree;
using FubarDocumentCompare.Demo.ViewModels;
using FubarDocumentCompare.Demo.Views;

namespace FubarDocumentCompare.Demo;

internal static class Program
{
    [STAThread]
    public static int Main(string[] args)
    {
        if (args.Length >= 2 && args[0] == "--screenshots") return Screenshots(args[1]);
        BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
        return 0;
    }

    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder.Configure<App>().UsePlatformDetect().WithInterFont().LogToTrace();

    /// <summary>
    /// Kör demot utan fönster (Avalonia headless med Skia) och sparar en bild per flik:
    /// ett genererat ark med rotation, skala, veck och flera etiketter, egna filer (testdata) och PDF-vyn.
    /// </summary>
    private static int Screenshots(string outDir)
    {
        Directory.CreateDirectory(outDir);
        AppBuilder.Configure<App>().UseSkia().WithInterFont()
            .UseHeadless(new AvaloniaHeadlessPlatformOptions { UseHeadlessDrawing = false })
            .SetupWithoutStarting();
        using var engine = new DocumentCompareEngine(new EngineOptions { MaxParallelJobs = 2 });
        var vm = new MainViewModel(engine);
        var window = new MainWindow { DataContext = vm, Width = 1600, Height = 1000 };
        window.Show();

        void Wait(Task t)
        {
            while (!t.IsCompleted)
            {
                Dispatcher.UIThread.RunJobs();
                AvaloniaHeadlessPlatform.ForceRenderTimerTick();
                Thread.Sleep(15);
            }
            t.GetAwaiter().GetResult();
        }

        void Shot(int tab, string name)
        {
            var tabs = window.GetVisualDescendants().OfType<Avalonia.Controls.TabControl>().First();
            tabs.SelectedIndex = tab;
            for (int i = 0; i < 20; i++)
            {
                Dispatcher.UIThread.RunJobs();
                AvaloniaHeadlessPlatform.ForceRenderTimerTick();
                Thread.Sleep(10);
            }
            var frame = window.CaptureRenderedFrame();
            frame?.Save(Path.Combine(outDir, name));
            Console.WriteLine($"{name}: {vm.Generator.Compare.Status}");
        }

        // 1. Demoark: 3 × 2 etiketter, 4,5° sned, 101,5 % skala, veck, CIS-skanner, två fel
        var g = vm.Generator;
        g.Rows = 3;
        g.Cols = 2;
        g.RotationDeg = 4.5;
        g.ScalePercent = 101.5;
        g.FoldProbability = 1.0;
        g.Difficulty = "hard";
        g.Errors = 2;
        g.Seed = 11;
        Wait(g.GenerateAndCompareCommand.ExecuteAsync(null));
        if (g.Compare.Findings.Count > 0) g.Compare.SelectedFinding = g.Compare.Findings[0];
        Shot(0, "1-demoark.png");

        // 2. Egna filer: testdatans etikett, PDF mot PDF
        vm.OwnFiles.UseTestDataCommand.Execute(null);
        Wait(vm.OwnFiles.RunCommand.ExecuteAsync(null));
        if (vm.OwnFiles.Compare.Findings.Count > 0) vm.OwnFiles.Compare.SelectedFinding = vm.OwnFiles.Compare.Findings[0];
        Shot(1, "2-egna-filer.png");

        // 3. PDF-vyn: smp.pdf med former, ord och streckkoder, utstansad
        if (OwnFilesViewModel.FindTestData() is { } root)
        {
            vm.Pdf.ShowWords = true;
            vm.Pdf.ShowBarcodes = true;
            Wait(vm.Pdf.OpenAsync(Path.Combine(root, "label", "smp.pdf")));
            Wait(Task.Delay(400));
            Shot(2, "3-pdf.png");
            vm.Pdf.CutOut = true;
            vm.Pdf.ShowWords = false;
            vm.Pdf.HideNonPrinting = true;
            Wait(Task.Delay(800));
            Shot(2, "4-pdf-utstansad.png");
        }
        window.Close();
        return 0;
    }
}
