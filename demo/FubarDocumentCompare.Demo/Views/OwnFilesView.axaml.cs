using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Platform.Storage;
using FubarDocumentCompare.Demo.ViewModels;

namespace FubarDocumentCompare.Demo.Views;

public partial class OwnFilesView : UserControl
{
    public OwnFilesView() => InitializeComponent();

    private OwnFilesViewModel Vm => (OwnFilesViewModel)DataContext!;

    private async void PickReference(object? sender, RoutedEventArgs e)
    {
        if (await Pick("Original", "*.pdf") is { } p) Vm.ReferencePath = p;
    }

    private async void PickSample(object? sender, RoutedEventArgs e)
    {
        if (await Pick("Prov", "*.tif", "*.tiff", "*.png", "*.jpg", "*.jpeg", "*.pdf") is { } p) Vm.SamplePath = p;
    }

    internal static async Task<string?> Pick(Control owner, string title, params string[] patterns)
    {
        var top = TopLevel.GetTopLevel(owner);
        if (top is null) return null;
        var files = await top.StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = title,
            AllowMultiple = false,
            FileTypeFilter = [new FilePickerFileType(title) { Patterns = patterns }],
        });
        return files.Count > 0 ? files[0].TryGetLocalPath() : null;
    }

    private Task<string?> Pick(string title, params string[] patterns) => Pick(this, title, patterns);
}
