using System.Runtime.InteropServices;

namespace FubarDocumentCompare.Interop;

/// <summary>P/Invoke-deklarationer som speglar fdc.h rad för rad. Använd de publika typerna i stället.</summary>
internal static unsafe class Native
{
    private const string Lib = "FubarDocumentCompare.Native";

    [StructLayout(LayoutKind.Sequential)]
    internal struct fdc_point { public double x, y; }

    [StructLayout(LayoutKind.Sequential)]
    internal struct fdc_engine_options { public uint size; public int max_parallel_jobs; public ulong cache_bytes; }

    [StructLayout(LayoutKind.Sequential)]
    internal struct fdc_reference_options
    {
        public uint size;
        public int page;
        public IntPtr box;
        public double dpi;
        public int profile;
        public IntPtr include_layers, exclude_layers, exclude_spots, tolerance_overrides_json;
        public IntPtr fonts;
        public IntPtr masks_json;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct fdc_source
    {
        public uint size;
        public int kind;
        public IntPtr path;
        public int page;
        public double dpi;
        public IntPtr data;
        public nuint data_len;
        public int width, height, stride, channels;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct fdc_compare_options
    {
        public uint size;
        public int expected_labels, rows, cols, multi;
        public double max_skew_deg, systematic_ratio;
        public int consensus, text_compare, color_calibrated, seed;
        public IntPtr data_check_path, data_check_row;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct fdc_document_options
    {
        public uint size;
        public int fixed_layout, compare_furniture, strict_typography, ignore_case, min_move_words;
        public double move_tol_mm;
        public IntPtr data_check_path;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct fdc_event
    {
        public uint size;
        public int type, stage;
        public double fraction;
        public int index;
        public double rotation_deg;
        public fdc_point c0, c1, c2, c3;
        public double width_px, height_px, dpi;
        public int preliminary, label_status, error;
        public IntPtr code, message;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct fdc_summary
    {
        public uint size;
        public int differences, labels_found, labels_expected, labels_pass, labels_warn, labels_fail;
        public int data_found, data_near, data_missing, data_mismatch;
        public double match_ratio;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct fdc_finding
    {
        public uint size;
        public int id, kind, severity, certainty, score, systematic, label, label_count, page;
        public double x_mm, y_mm, w_mm, h_mm;
        public IntPtr category, text_from, text_to, detail, reasons, fingerprint;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct fdc_label_info
    {
        public uint size;
        public int index, row, col, status;
        public double rotation_deg;
        public fdc_point c0, c1, c2, c3;
        public int folds;
        public double registration_rms_px;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct fdc_data_item
    {
        public uint size;
        public IntPtr field, expected;
        public int status;
        public IntPtr found_text, diff;
        public int count, page;
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void fdc_event_fn(IntPtr job, fdc_event* ev, IntPtr user);

    [DllImport(Lib)] internal static extern uint fdc_api_version();
    [DllImport(Lib)] internal static extern IntPtr fdc_version_string();
    [DllImport(Lib)] internal static extern IntPtr fdc_last_error();

    [DllImport(Lib)] internal static extern Status fdc_engine_create(fdc_engine_options* opt, out IntPtr engine);
    [DllImport(Lib)] internal static extern void fdc_engine_free(IntPtr engine);

    [DllImport(Lib)] internal static extern void fdc_reference_options_init(fdc_reference_options* opt);
    [DllImport(Lib)] internal static extern Status fdc_reference_open(IntPtr engine, byte* path, fdc_reference_options* opt, out IntPtr reference);
    [DllImport(Lib)] internal static extern Status fdc_reference_open_memory(IntPtr engine, byte* pdf, nuint len, fdc_reference_options* opt, out IntPtr reference);
    [DllImport(Lib)] internal static extern void fdc_reference_free(IntPtr reference);
    [DllImport(Lib)] internal static extern double fdc_reference_width_mm(IntPtr r);
    [DllImport(Lib)] internal static extern double fdc_reference_height_mm(IntPtr r);
    [DllImport(Lib)] internal static extern int fdc_reference_layer_count(IntPtr r);
    [DllImport(Lib)] internal static extern IntPtr fdc_reference_layer_name(IntPtr r, int i);
    [DllImport(Lib)] internal static extern int fdc_reference_layer_default_on(IntPtr r, int i);
    [DllImport(Lib)] internal static extern int fdc_reference_spot_count(IntPtr r);
    [DllImport(Lib)] internal static extern IntPtr fdc_reference_spot_name(IntPtr r, int i);
    [DllImport(Lib)] internal static extern int fdc_reference_barcode_count(IntPtr r);
    [DllImport(Lib)] internal static extern IntPtr fdc_reference_barcode_data(IntPtr r, int i);
    [DllImport(Lib)] internal static extern IntPtr fdc_reference_barcode_symbology(IntPtr r, int i);
    [DllImport(Lib)] internal static extern int fdc_reference_table_count(IntPtr r);
    [DllImport(Lib)] internal static extern int fdc_reference_braille_line_count(IntPtr r);
    [DllImport(Lib)] internal static extern IntPtr fdc_reference_braille_text(IntPtr r, int i);

    [DllImport(Lib)] internal static extern void fdc_source_init(fdc_source* s);
    [DllImport(Lib)] internal static extern void fdc_compare_options_init(fdc_compare_options* o);
    [DllImport(Lib)] internal static extern void fdc_document_options_init(fdc_document_options* o);

    [DllImport(Lib)] internal static extern Status fdc_job_start_compare(IntPtr engine, IntPtr reference, fdc_source* sample, fdc_compare_options* opt, IntPtr fn, IntPtr user, out IntPtr job);
    [DllImport(Lib)] internal static extern Status fdc_job_start_documents(IntPtr engine, byte* reference, byte* sample, fdc_document_options* opt, IntPtr fn, IntPtr user, out IntPtr job);
    [DllImport(Lib)] internal static extern Status fdc_job_start_data_check(IntPtr engine, byte* pdf, byte* expected, byte* row, IntPtr fn, IntPtr user, out IntPtr job);
    [DllImport(Lib)] internal static extern Status fdc_job_cancel(IntPtr job);
    [DllImport(Lib)] internal static extern Status fdc_job_wait(IntPtr job, int timeout_ms, out IntPtr result);
    [DllImport(Lib)] internal static extern void fdc_job_free(IntPtr job);

    [DllImport(Lib)] internal static extern int fdc_result_get_kind(IntPtr r);
    [DllImport(Lib)] internal static extern Status fdc_result_summary(IntPtr r, fdc_summary* out_);
    [DllImport(Lib)] internal static extern int fdc_result_finding_count(IntPtr r);
    [DllImport(Lib)] internal static extern Status fdc_result_finding(IntPtr r, int i, fdc_finding* out_);
    [DllImport(Lib)] internal static extern int fdc_result_label_count(IntPtr r);
    [DllImport(Lib)] internal static extern Status fdc_result_label(IntPtr r, int i, fdc_label_info* out_);
    [DllImport(Lib)] internal static extern int fdc_result_data_item_count(IntPtr r);
    [DllImport(Lib)] internal static extern Status fdc_result_data_item(IntPtr r, int i, fdc_data_item* out_);
    [DllImport(Lib)] internal static extern Status fdc_result_json(IntPtr r, out IntPtr buffer);
    [DllImport(Lib)] internal static extern Status fdc_result_report_html(IntPtr r, out IntPtr buffer);
    [DllImport(Lib)] internal static extern Status fdc_result_crop_png(IntPtr r, int finding, int side, int max_width, out IntPtr buffer);
    [DllImport(Lib)] internal static extern Status fdc_result_write_layers(IntPtr r, byte* path, double dpi);
    [DllImport(Lib)] internal static extern void fdc_result_free(IntPtr r);

    // ------------------------------------------------------------------ PDF-tjänster

    [StructLayout(LayoutKind.Sequential)]
    internal struct fdc_render_options
    {
        public uint size;
        public int page;
        public double dpi;
        public IntPtr box;
        public int x, y, width, height;
        public int pixels;
        public int antialias, annotations, printing;
        public IntPtr include_layers, exclude_layers, exclude_spots;
        public int hide_nonprinting;
        public IntPtr fonts;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct fdc_tile { public uint size; public int level, x, y, tile_size; }

    [StructLayout(LayoutKind.Sequential)]
    internal struct fdc_tile_grid { public uint size; public int columns, rows, width_px, height_px; public double dpi; }

    [StructLayout(LayoutKind.Sequential)]
    internal struct fdc_image_info
    {
        public uint size;
        public int width, height, stride, channels;
        public double dpi;
        public int x, y, full_width, full_height;
        public byte* data;
    }

    [DllImport(Lib)] internal static extern IntPtr fdc_buffer_data(IntPtr b);
    [DllImport(Lib)] internal static extern nuint fdc_buffer_size(IntPtr b);
    [DllImport(Lib)] internal static extern void fdc_buffer_free(IntPtr b);

    [DllImport(Lib)] internal static extern Status fdc_pdf_load(IntPtr engine, byte* data, nuint len, out IntPtr pdf);
    [DllImport(Lib)] internal static extern Status fdc_pdf_find(IntPtr engine, byte* sha256, out IntPtr pdf);
    [DllImport(Lib)] internal static extern IntPtr fdc_pdf_sha256(IntPtr pdf);
    [DllImport(Lib)] internal static extern void fdc_pdf_free(IntPtr pdf);
    [DllImport(Lib)] internal static extern Status fdc_pdf_describe(IntPtr engine, IntPtr pdf, IntPtr fonts, out IntPtr json);

    [DllImport(Lib)] internal static extern Status fdc_fontset_create(int fallback, out IntPtr set);
    [DllImport(Lib)] internal static extern Status fdc_fontset_add(IntPtr set, byte* data, nuint len, IntPtr aliases, out IntPtr result);
    [DllImport(Lib)] internal static extern Status fdc_fontset_combine(IntPtr a, IntPtr b, out IntPtr result);
    [DllImport(Lib)] internal static extern IntPtr fdc_fontset_hash(IntPtr set);
    [DllImport(Lib)] internal static extern Status fdc_fontset_describe(IntPtr set, out IntPtr json);
    [DllImport(Lib)] internal static extern void fdc_fontset_free(IntPtr set);
    [DllImport(Lib)] internal static extern Status fdc_font_validate(IntPtr engine, IntPtr pdf, int page, byte* font_name, byte* data, nuint len, out IntPtr json);
    [DllImport(Lib)] internal static extern void fdc_render_options_init(fdc_render_options* o);
    [DllImport(Lib)] internal static extern Status fdc_pdf_render(IntPtr engine, IntPtr pdf, fdc_render_options* o, out IntPtr image);
    [DllImport(Lib)] internal static extern Status fdc_pdf_tile_grid(IntPtr engine, IntPtr pdf, fdc_render_options* o, fdc_tile* t, fdc_tile_grid* out_);
    [DllImport(Lib)] internal static extern Status fdc_pdf_render_tile(IntPtr engine, IntPtr pdf, fdc_render_options* o, fdc_tile* t, out IntPtr image);
    [DllImport(Lib)] internal static extern Status fdc_pdf_overlays(IntPtr engine, IntPtr pdf, fdc_render_options* o, uint kinds, out IntPtr json);

    [DllImport(Lib)] internal static extern Status fdc_image_get_info(IntPtr image, fdc_image_info* out_);
    [DllImport(Lib)] internal static extern Status fdc_image_encode(IntPtr image, int format, int quality, out IntPtr buffer);
    [DllImport(Lib)] internal static extern Status fdc_image_load(byte* path, double dpi_override, out IntPtr image);
    [DllImport(Lib)] internal static extern void fdc_image_free(IntPtr image);

    /// <summary>Tar över en fdc_buffer: kopierar innehållet och frigör den.</summary>
    internal static byte[] TakeBuffer(IntPtr b)
    {
        try
        {
            var bytes = new byte[(int)fdc_buffer_size(b)];
            Marshal.Copy(fdc_buffer_data(b), bytes, 0, bytes.Length);
            return bytes;
        }
        finally
        {
            fdc_buffer_free(b);
        }
    }

    internal static string? Str(IntPtr p) => p == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(p);

    /// <summary>Kasta om status inte är OK, med trådens senaste feltext.</summary>
    internal static void Check(Status s)
    {
        if (s == Status.Ok) return;
        string msg = Str(fdc_last_error()) ?? s.ToString();
        if (s == Status.Cancelled) throw new OperationCanceledException(msg);
        throw new DocumentCompareException(s, msg);
    }
}

/// <summary>UTF-8-strängar för ett anrop; frigörs när blocket lämnas.</summary>
internal sealed class Utf8Scope : IDisposable
{
    private readonly List<IntPtr> _owned = new();

    public IntPtr Add(string? s)
    {
        if (s is null) return IntPtr.Zero;
        var p = Marshal.StringToCoTaskMemUTF8(s);
        _owned.Add(p);
        return p;
    }

    public unsafe byte* Ptr(string? s) => (byte*)Add(s);

    public void Dispose()
    {
        foreach (var p in _owned) Marshal.FreeCoTaskMem(p);
        _owned.Clear();
    }
}
