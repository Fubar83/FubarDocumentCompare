using Avalonia.Controls;
using Avalonia.Interactivity;
using FubarDocumentCompare.Demo.ViewModels;

namespace FubarDocumentCompare.Demo.Views;

public partial class PdfView : UserControl
{
    public PdfView() => InitializeComponent();

    private PdfViewModel Vm => (PdfViewModel)DataContext!;

    private async void Open(object? sender, RoutedEventArgs e)
    {
        if (await OwnFilesView.Pick(this, "PDF", "*.pdf") is { } p) await Vm.OpenAsync(p);
    }

    private async void OpenSample(object? sender, RoutedEventArgs e)
    {
        if (OwnFilesViewModel.FindTestData() is { } root) await Vm.OpenAsync(Path.Combine(root, "label", "smp.pdf"));
    }

    // En ISO 19593-patch från realworld-testet (core/build/windows/realworld), om det har körts
    private async void OpenGwg(object? sender, RoutedEventArgs e)
    {
        if (OwnFilesViewModel.FindTestData() is not { } root) return;
        var dir = Path.Combine(Path.GetDirectoryName(root)!, "core", "build", "windows", "realworld", "gwg-steps");
        var patch = Directory.Exists(dir) ? Directory.EnumerateFiles(dir, "Patch PS-001-02G.pdf", SearchOption.AllDirectories).FirstOrDefault() : null;
        if (patch is not null) await Vm.OpenAsync(patch);
        else Vm.Status = "Kör realworld-testet först (ctest -L realworld) för att hämta GWG:s patchar.";
    }
}
