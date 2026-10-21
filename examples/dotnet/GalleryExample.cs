// Ett galleri av bilder ur en PDF: rutor, varianter, stanslinje, lackmask, utstansad etikett (genomskinlig
// utanför stanslinjen), beskuren till stanslinjen, annoterad med bildkartan, och en bild per lager.
// Masker och utstansning görs här i exemplet med överläggens former (mm → px); biblioteket levererar
// bilderna och formerna.
using System.Buffers.Binary;
using System.IO.Compression;
using FubarDocumentCompare;
using FubarDocumentCompare.Imaging;
using FubarDocumentCompare.Pdf;

namespace Examples;

internal static class Gallery
{
    public static async Task Run(DocumentCompareEngine engine, string pdfPath, string outDir, double dpi = 200)
    {
        Directory.CreateDirectory(outDir);
        var pdf = PdfSource.FromFile(pdfPath);
        var meta = await engine.PdfInspector.GetMetadataAsync(pdf);
        var page = meta.Pages[0];
        Console.WriteLine($"{Path.GetFileName(pdfPath)}: {page.Layers.Count} lager, dekorfärger: " +
                          string.Join(", ", page.Spots.Select(s => s.Role is null ? s.Name : $"{s.Name} ({s.Role})")));

        async Task<Canvas> Render(RenderOptions o) { using var img = await engine.PdfRenderer.RenderAsync(pdf, o); return Canvas.From(engine, img); }
        var saved = new List<Canvas>();
        void Save(Canvas c, string name) { c.SavePng(Path.Combine(outDir, name)); saved.Add(c); Console.WriteLine($"  {name} ({c.Width} × {c.Height})"); }

        var trim = new RenderOptions { Dpi = dpi, Box = PageBox.Trim };
        var printing = new PdfVariant { HideNonPrinting = true };

        // 1–4: rutor och varianter
        Save(await Render(trim), "01-trim.png");
        Save(await Render(trim with { Box = PageBox.Media }), "02-media.png");
        Save(await Render(trim with { Variant = printing }), "03-bara-tryck.png");
        var varnish = page.Spots.Where(s => s.Role == "varnish").Select(s => s.Name).ToList();
        if (varnish.Count > 0) Save(await Render(trim with { Variant = new PdfVariant { ExcludeSpots = varnish } }), "04-utan-lack.png");

        // Formerna (mm från trimrutans hörn) → px i trimrutans raster
        var map = await engine.PdfInspector.GetOverlaysAsync(pdf, new OverlayOptions { Box = PageBox.Trim, Kinds = OverlayKinds.All });
        double px = dpi / 25.4;
        List<(double X, double Y)[]> Shapes(string role) =>
            map.Spots.Where(s => s.Role == role).SelectMany(s => s.Polygons).Select(p => p.Select(q => (q.X * px, q.Y * px)).ToArray()).ToList();
        var cuts = Shapes("dieline");
        var varnishShapes = Shapes("varnish");
        var size = await Render(trim with { Dpi = dpi });

        // 5: stanslinjen ensam
        if (cuts.Count > 0)
        {
            var line = Canvas.Blank(size.Width, size.Height, 255, 255, 255, 255);
            foreach (var c in cuts) line.Stroke(c, 220, 30, 30, 3, closed: true);
            Save(line, "05-stanslinje.png");
        }

        // 6: lackmask (svart = lack)
        if (varnishShapes.Count > 0)
        {
            var mask = Canvas.Blank(size.Width, size.Height, 255, 255, 255, 255);
            mask.Fill(varnishShapes, 0, 0, 0, 255);
            Save(mask, "06-lackmask.png");
        }

        // 7–8: utstansad etikett: tryck, genomskinligt utanför stanslinjen; sedan beskuren till stanslinjen
        if (cuts.Count > 0)
        {
            var cutout = await Render(trim with { Variant = printing, Pixels = PixelFormat.Rgba });
            cutout.KeepInside(cuts);
            Save(cutout, "07-utstansad.png");
            Save(cutout.Crop(Canvas.Bounds(cuts), pad: 2), "08-utstansad-beskuren.png");
        }

        // 9: annoterad: ord (blå), streckkoder (grön), stanslinje (röd), lack (gul)
        var ann = await Render(trim);
        foreach (var w in map.Words) ann.Rect(w.Box.X * px, w.Box.Y * px, w.Box.Width * px, w.Box.Height * px, 40, 90, 220, 1);
        foreach (var b in map.Barcodes) ann.Rect(b.Box.X * px, b.Box.Y * px, b.Box.Width * px, b.Box.Height * px, 20, 160, 60, 3);
        foreach (var v in varnishShapes) ann.Stroke(v, 230, 180, 0, 2, closed: true);
        foreach (var c in cuts) ann.Stroke(c, 220, 30, 30, 2, closed: true);
        Save(ann, "09-annoterad.png");

        // 10: gråskala utan kantutjämning
        Save(await Render(trim with { Pixels = PixelFormat.Gray, Antialias = false }), "10-gra-utan-kantutjamning.png");

        // 11: ett lager i taget
        foreach (var layer in page.Layers)
        {
            var name = new string(layer.Name.Select(ch => char.IsLetterOrDigit(ch) ? ch : '-').ToArray());
            var step = layer.ProcessingStep is { } s ? $" [{s.Group}/{s.Type}]" : "";
            Save(await Render(trim with { Variant = new PdfVariant { IncludeLayers = [layer.Name] } }), $"11-lager-{name}.png");
            if (step != "") Console.WriteLine($"    bearbetningssteg{step}");
        }

        // Översikt: alla bilder i filordning, tre per rad, på grått (så att genomskinligt syns)
        var sheet = Canvas.Sheet(saved, columns: 3, gap: 12);
        sheet.SavePng(Path.Combine(outDir, "00-oversikt.png"));
        Console.WriteLine($"  00-oversikt.png ({sheet.Width} × {sheet.Height})");
    }
}

/// <summary>RGBA-pixlar med enkel ritning och PNG-kodning (bara för exemplet).</summary>
internal sealed class Canvas
{
    private Canvas(int w, int h, byte[] rgba) => (Width, Height, Pixels) = (w, h, rgba);

    public int Width { get; }
    public int Height { get; }
    public byte[] Pixels { get; }

    public static Canvas Blank(int w, int h, byte r, byte g, byte b, byte a)
    {
        var p = new byte[w * h * 4];
        for (int i = 0; i < p.Length; i += 4) (p[i], p[i + 1], p[i + 2], p[i + 3]) = (r, g, b, a);
        return new Canvas(w, h, p);
    }

    public static Canvas From(DocumentCompareEngine engine, Image img)
    {
        var src = engine.ImageCodec.CopyPixels(img);
        var p = new byte[img.Width * img.Height * 4];
        for (int i = 0, n = img.Width * img.Height; i < n; i++)
        {
            switch (img.Channels)
            {
                case 4: Array.Copy(src, i * 4, p, i * 4, 4); break;
                case 1: p[i * 4] = p[i * 4 + 1] = p[i * 4 + 2] = src[i]; p[i * 4 + 3] = 255; break;
                default: Array.Copy(src, i * 3, p, i * 4, 3); p[i * 4 + 3] = 255; break;
            }
        }
        return new Canvas(img.Width, img.Height, p);
    }

    private void Set(int x, int y, byte r, byte g, byte b, byte a)
    {
        if ((uint)x >= Width || (uint)y >= Height) return;
        int i = (y * Width + x) * 4;
        (Pixels[i], Pixels[i + 1], Pixels[i + 2], Pixels[i + 3]) = (r, g, b, a);
    }

    /// <summary>Fyller polygonerna med jämn-udda-regeln (hål i former blir hål).</summary>
    public void Fill(List<(double X, double Y)[]> polys, byte r, byte g, byte b, byte a) => ForInside(polys, (x, y) => Set(x, y, r, g, b, a));

    /// <summary>Gör allt utanför polygonerna genomskinligt.</summary>
    public void KeepInside(List<(double X, double Y)[]> polys)
    {
        var inside = new bool[Width * Height];
        ForInside(polys, (x, y) => inside[y * Width + x] = true);
        for (int i = 0; i < inside.Length; i++)
            if (!inside[i]) Pixels[i * 4 + 3] = 0;
    }

    private void ForInside(List<(double X, double Y)[]> polys, Action<int, int> at)
    {
        var xs = new List<double>();
        for (int y = 0; y < Height; y++)
        {
            double sy = y + 0.5;
            xs.Clear();
            foreach (var p in polys)
                for (int i = 0; i < p.Length; i++)
                {
                    var (a, b) = (p[i], p[(i + 1) % p.Length]);
                    if ((a.Y <= sy) != (b.Y <= sy)) xs.Add(a.X + (sy - a.Y) / (b.Y - a.Y) * (b.X - a.X));
                }
            xs.Sort();
            for (int k = 0; k + 1 < xs.Count; k += 2)
                for (int x = Math.Max(0, (int)Math.Ceiling(xs[k] - 0.5)); x < Math.Min(Width, (int)Math.Ceiling(xs[k + 1] - 0.5)); x++)
                    at(x, y);
        }
    }

    public void Stroke((double X, double Y)[] pts, byte r, byte g, byte b, int width, bool closed)
    {
        int n = closed ? pts.Length : pts.Length - 1;
        for (int i = 0; i < n; i++) Line(pts[i], pts[(i + 1) % pts.Length], r, g, b, width);
    }

    public void Rect(double x, double y, double w, double h, byte r, byte g, byte b, int width) =>
        Stroke([(x, y), (x + w, y), (x + w, y + h), (x, y + h)], r, g, b, width, closed: true);

    private void Line((double X, double Y) a, (double X, double Y) b, byte r, byte g, byte bl, int width)
    {
        double len = Math.Max(Math.Abs(b.X - a.X), Math.Abs(b.Y - a.Y));
        int steps = Math.Max(1, (int)Math.Ceiling(len));
        int half = width / 2;
        for (int s = 0; s <= steps; s++)
        {
            double t = (double)s / steps;
            int cx = (int)Math.Round(a.X + (b.X - a.X) * t), cy = (int)Math.Round(a.Y + (b.Y - a.Y) * t);
            for (int dy = -half; dy <= half; dy++)
                for (int dx = -half; dx <= half; dx++) Set(cx + dx, cy + dy, r, g, bl, 255);
        }
    }

    public static (double X0, double Y0, double X1, double Y1) Bounds(List<(double X, double Y)[]> polys) =>
        (polys.SelectMany(p => p).Min(q => q.X), polys.SelectMany(p => p).Min(q => q.Y),
         polys.SelectMany(p => p).Max(q => q.X), polys.SelectMany(p => p).Max(q => q.Y));

    public Canvas Crop((double X0, double Y0, double X1, double Y1) box, int pad)
    {
        int x0 = Math.Clamp((int)Math.Floor(box.X0) - pad, 0, Width - 1), y0 = Math.Clamp((int)Math.Floor(box.Y0) - pad, 0, Height - 1);
        int x1 = Math.Clamp((int)Math.Ceiling(box.X1) + pad, x0 + 1, Width), y1 = Math.Clamp((int)Math.Ceiling(box.Y1) + pad, y0 + 1, Height);
        int w = x1 - x0, h = y1 - y0;
        var p = new byte[w * h * 4];
        for (int y = 0; y < h; y++) Array.Copy(Pixels, ((y0 + y) * Width + x0) * 4, p, y * w * 4, w * 4);
        return new Canvas(w, h, p);
    }

    public static Canvas Sheet(List<Canvas> images, int columns, int gap)
    {
        int cw = images.Max(i => i.Width), ch = images.Max(i => i.Height), rows = (images.Count + columns - 1) / columns;
        var sheet = Blank(columns * cw + (columns + 1) * gap, rows * ch + (rows + 1) * gap, 200, 200, 200, 255);
        for (int k = 0; k < images.Count; k++)
        {
            var img = images[k];
            int ox = gap + (k % columns) * (cw + gap), oy = gap + (k / columns) * (ch + gap);
            for (int y = 0; y < img.Height; y++)
                for (int x = 0; x < img.Width; x++)
                {
                    int s = (y * img.Width + x) * 4, d = ((oy + y) * sheet.Width + ox + x) * 4;
                    int a = img.Pixels[s + 3];
                    for (int c = 0; c < 3; c++) sheet.Pixels[d + c] = (byte)((img.Pixels[s + c] * a + sheet.Pixels[d + c] * (255 - a)) / 255);
                }
        }
        return sheet;
    }

    public void SavePng(string path)
    {
        using var f = File.Create(path);
        f.Write([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]);
        var ihdr = new byte[13];
        BinaryPrimitives.WriteInt32BigEndian(ihdr, Width);
        BinaryPrimitives.WriteInt32BigEndian(ihdr.AsSpan(4), Height);
        ihdr[8] = 8;   // bitdjup
        ihdr[9] = 6;   // RGBA
        Chunk(f, "IHDR", ihdr);
        using var raw = new MemoryStream();
        using (var z = new ZLibStream(raw, CompressionLevel.Fastest, leaveOpen: true))
            for (int y = 0; y < Height; y++)
            {
                z.WriteByte(0);
                z.Write(Pixels, y * Width * 4, Width * 4);
            }
        Chunk(f, "IDAT", raw.ToArray());
        Chunk(f, "IEND", []);
    }

    private static void Chunk(Stream s, string type, byte[] data)
    {
        Span<byte> head = stackalloc byte[8];
        BinaryPrimitives.WriteInt32BigEndian(head, data.Length);
        for (int i = 0; i < 4; i++) head[4 + i] = (byte)type[i];
        s.Write(head);
        s.Write(data);
        uint crc = 0xFFFFFFFF;
        foreach (var x in head[4..]) crc = CrcTable[(crc ^ x) & 0xFF] ^ (crc >> 8);
        foreach (var x in data) crc = CrcTable[(crc ^ x) & 0xFF] ^ (crc >> 8);
        Span<byte> tail = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(tail, crc ^ 0xFFFFFFFF);
        s.Write(tail);
    }

    private static readonly uint[] CrcTable = Enumerable.Range(0, 256).Select(n =>
    {
        uint c = (uint)n;
        for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
        return c;
    }).ToArray();
}
