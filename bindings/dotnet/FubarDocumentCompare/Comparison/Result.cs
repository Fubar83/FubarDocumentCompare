using System.Buffers.Binary;
using System.IO.Compression;

namespace FubarDocumentCompare.Comparison;

/// <summary>
/// Ett färdigt resultat (oföränderligt). Fynd, etiketter och datafält är kopierade till .NET-objekt;
/// handtaget behövs bara för utsnitt och export.
/// </summary>
public sealed unsafe class Result : IDisposable
{
    private IntPtr _h;

    internal Result(IntPtr h)
    {
        _h = h;
        Kind = (ResultKind)Native.fdc_result_get_kind(h);
        var s = new Native.fdc_summary { size = (uint)sizeof(Native.fdc_summary) };
        Native.Check(Native.fdc_result_summary(h, &s));
        Summary = new Summary(s.differences, s.labels_found, s.labels_expected, s.labels_pass, s.labels_warn, s.labels_fail,
                              s.data_found, s.data_near, s.data_missing, s.data_mismatch, s.match_ratio);

        var findings = new Finding[Native.fdc_result_finding_count(h)];
        for (int i = 0; i < findings.Length; i++)
        {
            var f = new Native.fdc_finding { size = (uint)sizeof(Native.fdc_finding) };
            Native.Check(Native.fdc_result_finding(h, i, &f));
            findings[i] = new Finding(i, f.id, (FindingKind)f.kind, (Severity)f.severity, (Certainty)f.certainty, f.score,
                f.systematic != 0, f.label, f.label_count, f.page, f.x_mm, f.y_mm, f.w_mm, f.h_mm,
                Native.Str(f.category) ?? "", Native.Str(f.text_from), Native.Str(f.text_to), Native.Str(f.detail) ?? "",
                (Native.Str(f.reasons) ?? "").Split('\n', StringSplitOptions.RemoveEmptyEntries), Native.Str(f.fingerprint) ?? "");
        }
        Findings = findings;

        var labels = new LabelInfo[Native.fdc_result_label_count(h)];
        for (int i = 0; i < labels.Length; i++)
        {
            var l = new Native.fdc_label_info { size = (uint)sizeof(Native.fdc_label_info) };
            Native.Check(Native.fdc_result_label(h, i, &l));
            labels[i] = new LabelInfo(l.index, l.row, l.col, (LabelStatus)l.status, l.rotation_deg,
                new[] { Job.P(l.c0), Job.P(l.c1), Job.P(l.c2), Job.P(l.c3) }, l.folds, l.registration_rms_px);
        }
        Labels = labels;

        var items = new DataItem[Native.fdc_result_data_item_count(h)];
        for (int i = 0; i < items.Length; i++)
        {
            var d = new Native.fdc_data_item { size = (uint)sizeof(Native.fdc_data_item) };
            Native.Check(Native.fdc_result_data_item(h, i, &d));
            items[i] = new DataItem(Native.Str(d.field) ?? "", Native.Str(d.expected) ?? "", (DataStatus)d.status,
                                    Native.Str(d.found_text), Native.Str(d.diff), d.count, d.page);
        }
        DataItems = items;
    }

    private IntPtr Handle => _h != IntPtr.Zero ? _h : throw new ObjectDisposedException(nameof(Result));

    public ResultKind Kind { get; }
    public Summary Summary { get; }
    public IReadOnlyList<Finding> Findings { get; }
    public IReadOnlyList<LabelInfo> Labels { get; }
    public IReadOnlyList<DataItem> DataItems { get; }

    /// <summary>Utsnitt kring ett fynd ur original- eller provbilden (etikettläge).</summary>
    public RgbImage RenderCrop(int findingIndex, Side side, int maxWidth = 600)
    {
        Native.Check(Native.fdc_result_render_crop(Handle, findingIndex, (int)side, maxWidth, null, 0, out int w, out int h));
        var buf = new byte[w * h * 3];
        fixed (byte* p = buf)
            Native.Check(Native.fdc_result_render_crop(Handle, findingIndex, (int)side, maxWidth, p, (nuint)buf.Length, out w, out h));
        return new RgbImage(w, h, buf);
    }

    public void WriteReport(string htmlPath)
    {
        using var s = new Utf8Scope();
        Native.Check(Native.fdc_result_write_report(Handle, s.Ptr(htmlPath)));
    }

    /// <summary>Samma result.json som printcmp-cli skriver.</summary>
    public void WriteJson(string jsonPath)
    {
        using var s = new Utf8Scope();
        Native.Check(Native.fdc_result_write_json(Handle, s.Ptr(jsonPath)));
    }

    public void WriteLayers(string directory, double dpi = 200)
    {
        using var s = new Utf8Scope();
        Native.Check(Native.fdc_result_write_layers(Handle, s.Ptr(directory), dpi));
    }

    public void Dispose()
    {
        var h = Interlocked.Exchange(ref _h, IntPtr.Zero);
        if (h != IntPtr.Zero) Native.fdc_result_free(h);
        GC.SuppressFinalize(this);
    }

    ~Result() => Dispose();
}

/// <summary>8-bitars RGB, rad för rad utan utfyllnad.</summary>
public sealed class RgbImage
{
    public RgbImage(int width, int height, byte[] pixels) => (Width, Height, Pixels) = (width, height, pixels);

    public int Width { get; }
    public int Height { get; }
    public byte[] Pixels { get; }

    /// <summary>Kodar bilden som PNG (utan externa beroenden).</summary>
    public byte[] ToPng()
    {
        using var ms = new MemoryStream();
        ms.Write(stackalloc byte[] { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A });
        Span<byte> ihdr = stackalloc byte[13];
        BinaryPrimitives.WriteInt32BigEndian(ihdr, Width);
        BinaryPrimitives.WriteInt32BigEndian(ihdr[4..], Height);
        ihdr[8] = 8;   // bitdjup
        ihdr[9] = 2;   // RGB
        Chunk(ms, "IHDR", ihdr);
        using (var raw = new MemoryStream())
        {
            using (var z = new ZLibStream(raw, CompressionLevel.Fastest, leaveOpen: true))
            {
                int row = Width * 3;
                for (int y = 0; y < Height; y++)
                {
                    z.WriteByte(0);   // filter: inget
                    z.Write(Pixels, y * row, row);
                }
            }
            Chunk(ms, "IDAT", raw.GetBuffer().AsSpan(0, (int)raw.Length));
        }
        Chunk(ms, "IEND", ReadOnlySpan<byte>.Empty);
        return ms.ToArray();
    }

    private static void Chunk(Stream s, string type, ReadOnlySpan<byte> data)
    {
        Span<byte> head = stackalloc byte[8];
        BinaryPrimitives.WriteInt32BigEndian(head, data.Length);
        for (int i = 0; i < 4; i++) head[4 + i] = (byte)type[i];
        s.Write(head);
        s.Write(data);
        uint crc = Crc32(head[4..], 0xFFFFFFFF);
        crc = Crc32(data, crc) ^ 0xFFFFFFFF;
        Span<byte> tail = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(tail, crc);
        s.Write(tail);
    }

    private static readonly uint[] s_crc = Enumerable.Range(0, 256).Select(n =>
    {
        uint c = (uint)n;
        for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
        return c;
    }).ToArray();

    private static uint Crc32(ReadOnlySpan<byte> data, uint crc)
    {
        foreach (var b in data) crc = s_crc[(crc ^ b) & 0xFF] ^ (crc >> 8);
        return crc;
    }
}
