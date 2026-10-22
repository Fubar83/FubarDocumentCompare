using Avalonia.Controls;
using Avalonia.Interactivity;
using FubarDocumentCompare.Demo.ViewModels;

namespace FubarDocumentCompare.Demo.Views;

public partial class PeersView : UserControl
{
    public PeersView() => InitializeComponent();

    private PeersViewModel Vm => (PeersViewModel)DataContext!;

    private async void OpenScan(object? sender, RoutedEventArgs e)
    {
        if (await OwnFilesView.Pick(this, "Scan", "*.tif", "*.tiff", "*.png", "*.jpg", "*.jpeg") is { } p) await Vm.LoadAsync(p);
    }
}
