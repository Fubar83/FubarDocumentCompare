using System.Runtime.InteropServices;

namespace FubarDocumentCompare.Interop;

/// <summary>fdc_pdf: en PDF i motorns cache, identifierad av sin SHA-256.</summary>
internal sealed class PdfHandle : SafeHandle
{
    public PdfHandle() : base(IntPtr.Zero, ownsHandle: true) { }
    public override bool IsInvalid => handle == IntPtr.Zero;
    protected override bool ReleaseHandle()
    {
        Native.fdc_pdf_free(handle);
        return true;
    }
    internal static PdfHandle Wrap(IntPtr p)
    {
        var h = new PdfHandle();
        h.SetHandle(p);
        return h;
    }
}

/// <summary>fdc_image: pixlar i inbyggt minne.</summary>
internal sealed class ImageHandle : SafeHandle
{
    private readonly long _bytes;

    private ImageHandle(long bytes) : base(IntPtr.Zero, ownsHandle: true) => _bytes = bytes;
    public override bool IsInvalid => handle == IntPtr.Zero;

    protected override bool ReleaseHandle()
    {
        Native.fdc_image_free(handle);
        if (_bytes > 0) GC.RemoveMemoryPressure(_bytes);
        return true;
    }

    /// <summary>Tar över handtaget och talar om för skräpsamlaren hur mycket minne det håller.</summary>
    internal static ImageHandle Wrap(IntPtr p, long bytes)
    {
        var h = new ImageHandle(bytes);
        h.SetHandle(p);
        if (bytes > 0) GC.AddMemoryPressure(bytes);
        return h;
    }
}

/// <summary>fdc_fontset: en oföränderlig uppsättning fontfiler.</summary>
internal sealed class FontSetHandle : SafeHandle
{
    public FontSetHandle() : base(IntPtr.Zero, ownsHandle: true) { }
    public override bool IsInvalid => handle == IntPtr.Zero;
    protected override bool ReleaseHandle()
    {
        Native.fdc_fontset_free(handle);
        return true;
    }
    internal static FontSetHandle Wrap(IntPtr p)
    {
        var h = new FontSetHandle();
        h.SetHandle(p);
        return h;
    }
}

/// <summary>Håller ett SafeHandle vid liv under ett inbyggt anrop.</summary>
internal readonly struct HandleLease : IDisposable
{
    private readonly SafeHandle? _h;
    public IntPtr Pointer { get; }

    public HandleLease(SafeHandle? h)
    {
        _h = null;
        Pointer = IntPtr.Zero;
        if (h is null) return;
        bool added = false;
        h.DangerousAddRef(ref added);
        if (!added) throw new ObjectDisposedException(h.GetType().Name);
        _h = h;
        Pointer = h.DangerousGetHandle();
    }

    public void Dispose() => _h?.DangerousRelease();
}
