using Avalonia.Controls;
using Avalonia.Interactivity;
using FubarDocumentCompare.Demo.ViewModels;

namespace FubarDocumentCompare.Demo.Views;

public partial class ImageLabView : UserControl
{
    public ImageLabView() => InitializeComponent();

    private ImageLabViewModel Vm => (ImageLabViewModel)DataContext!;

    private async void OpenScan(object? sender, RoutedEventArgs e)
    {
        if (await OwnFilesView.Pick(this, "Scan", "*.tif", "*.tiff", "*.png", "*.jpg", "*.jpeg") is { } p) await Vm.LoadScanAsync(p);
    }

    private async void OpenPdf(object? sender, RoutedEventArgs e)
    {
        if (await OwnFilesView.Pick(this, "PDF", "*.pdf") is { } p) await Vm.LoadPdfAsync(p);
    }

    // Med ett demoark är originalet känt; annars väljs det
    private async void Locate(object? sender, RoutedEventArgs e)
    {
        try
        {
            await Vm.LocateAsync();
        }
        catch (InvalidOperationException)
        {
        }
        if (Vm.Status.StartsWith("Ladda") && await OwnFilesView.Pick(this, "Original", "*.pdf") is { } p) await Vm.LocateAsync(p);
    }
}
