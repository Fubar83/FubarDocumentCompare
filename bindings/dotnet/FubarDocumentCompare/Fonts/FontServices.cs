using System.Text.Json;
using FubarDocumentCompare.Pdf;

namespace FubarDocumentCompare.Fonts;

/// <summary>Skapar och ändrar typsnittsuppsättningar. Tillståndslös och trådsäker.</summary>
public interface IFontSetLoader
{
    FontSet Create(FontFallback fallback = FontFallback.Builtin);

    /// <summary>Ny uppsättning med <paramref name="fonts"/>. Kastar <see cref="DocumentCompareException"/>
    /// (FileFormat) för en fil som inte är ett TrueType/OpenType-typsnitt.</summary>
    FontSet Create(IEnumerable<FontSource> fonts, FontFallback fallback = FontFallback.Builtin);

    /// <summary>Ny uppsättning = <paramref name="set"/> + <paramref name="font"/>; <paramref name="set"/> ändras inte.</summary>
    FontSet Add(FontSet set, FontSource font);

    /// <summary>Ny uppsättning med <paramref name="first"/>s typsnitt före <paramref name="second"/>s
    /// (och <paramref name="first"/>s reservval), t.ex. jobbets extra typsnitt före kundens.</summary>
    FontSet Combine(FontSet first, FontSet second);
}

/// <summary>Kontrollerar att en fontfil stämmer med ett typsnitt som inte är inbäddat i en PDF.</summary>
public interface IFontValidator
{
    /// <summary>Jämför stegbredderna PDF:en räknar med mot filens och letar efter tecken som saknas.
    /// <paramref name="page"/> 0 = alla sidor.</summary>
    Task<FontCheck> ValidateAsync(PdfSource pdf, string fontName, FontSource candidate, int page = 0,
                                  CancellationToken cancellationToken = default);
}

internal sealed class FontSetLoader : IFontSetLoader
{
    public FontSet Create(FontFallback fallback = FontFallback.Builtin)
    {
        Native.Check(Native.fdc_fontset_create((int)fallback, out var p));
        return Wrap(p);
    }

    public FontSet Create(IEnumerable<FontSource> fonts, FontFallback fallback = FontFallback.Builtin)
    {
        ArgumentNullException.ThrowIfNull(fonts);
        var set = Create(fallback);
        foreach (var f in fonts)
        {
            var next = Add(set, f);
            set.Dispose();
            set = next;
        }
        return set;
    }

    public unsafe FontSet Add(FontSet set, FontSource font)
    {
        ArgumentNullException.ThrowIfNull(set);
        ArgumentNullException.ThrowIfNull(font);
        using var lease = new HandleLease(set.Handle);
        using var s = new Utf8Scope();
        var aliases = s.Add(font.Aliases is { Count: > 0 } a ? string.Join(';', a) : null);
        IntPtr p;
        fixed (byte* d = font.Data.Span)
            Native.Check(Native.fdc_fontset_add(lease.Pointer, d, (nuint)font.Data.Length, aliases, out p));
        return Wrap(p);
    }

    public FontSet Combine(FontSet first, FontSet second)
    {
        ArgumentNullException.ThrowIfNull(first);
        ArgumentNullException.ThrowIfNull(second);
        using var a = new HandleLease(first.Handle);
        using var b = new HandleLease(second.Handle);
        Native.Check(Native.fdc_fontset_combine(a.Pointer, b.Pointer, out var p));
        return Wrap(p);
    }

    private static FontSet Wrap(IntPtr p)
    {
        var handle = FontSetHandle.Wrap(p);
        try
        {
            Native.Check(Native.fdc_fontset_describe(p, out var buffer));
            var d = JsonSerializer.Deserialize(Native.TakeBuffer(buffer), FontJsonContext.Default.FontSetDescription)
                    ?? throw new DocumentCompareException(Status.Internal, "tom beskrivning av typsnittsuppsättningen");
            return new FontSet(handle, d);
        }
        catch
        {
            handle.Dispose();
            throw;
        }
    }
}

internal sealed class FontValidator(DocumentCompareEngine engine, PdfResolver resolver) : IFontValidator
{
    public async Task<FontCheck> ValidateAsync(PdfSource pdf, string fontName, FontSource candidate, int page = 0,
                                               CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrEmpty(fontName);
        ArgumentNullException.ThrowIfNull(candidate);
        using var h = await resolver.ResolveAsync(pdf, cancellationToken).ConfigureAwait(false);
        var json = Validate(h, fontName, candidate, page);
        return JsonSerializer.Deserialize(json, FontJsonContext.Default.FontCheck)
               ?? throw new DocumentCompareException(Status.Internal, "tom fontkontroll");
    }

    private unsafe byte[] Validate(PdfHandle h, string fontName, FontSource candidate, int page)
    {
        using var s = new Utf8Scope();
        IntPtr buffer;
        fixed (byte* d = candidate.Data.Span)
            Native.Check(Native.fdc_font_validate(engine.Handle, h.DangerousGetHandle(), page, s.Ptr(fontName), d,
                                                  (nuint)candidate.Data.Length, out buffer));
        return Native.TakeBuffer(buffer);
    }
}
