// Jobb: start, händelser (återanrop eller poll), avbrott och väntan på resultatet.
#include "internal.hpp"

namespace {

compare::DataCheckOptions data_opts(const char* path, const char* row) {
    compare::DataCheckOptions d;
    d.path = path;
    if (row && *row) {
        std::string r = row;
        if (text::is_digit_str(r)) d.row = std::stoi(r);
        else d.row_match = r;
    }
    return d;
}

}  // namespace

extern "C" {

FDC_API fdc_status fdc_job_start_compare(fdc_engine* e, fdc_reference* ref, const fdc_source* sample, const fdc_compare_options* opt,
                                      fdc_event_fn fn, void* user, fdc_job** out) {
    return guard([&] {
        if (!e || !ref || !sample || !out) return set_error(FDC_ERR_INVALID_ARGUMENT, "motor, referens, prov eller out saknas");
        compare::SampleSource src;
        switch (sample->kind) {
            case FDC_SOURCE_PDF_FILE: src.kind = compare::SampleSource::Kind::Pdf; break;
            case FDC_SOURCE_PDF_MEMORY: {
                src.kind = compare::SampleSource::Kind::Pdf;
                const auto* p = static_cast<const unsigned char*>(sample->data);
                if (!p) return set_error(FDC_ERR_INVALID_ARGUMENT, "PDF-data saknas");
                src.pdf_bytes.assign(p, p + sample->data_len);
                break;
            }
            case FDC_SOURCE_IMAGE_BUFFER: {
                src.kind = compare::SampleSource::Kind::Image;
                auto s = image::from_buffer(static_cast<const unsigned char*>(sample->data), sample->width, sample->height,
                                            sample->stride, sample->channels, sample->dpi);
                src.rgb = s.rgb;
                break;
            }
            default: src.kind = compare::SampleSource::Kind::Scan;
        }
        src.path = sample->path ? sample->path : "";
        src.page = sample->page > 0 ? sample->page : 1;
        src.dpi = sample->dpi;
        fdc_compare_options d;
        fdc_compare_options_init(&d);
        const fdc_compare_options& o = opt ? *opt : d;
        compare::CompareOptions co;
        co.instance_mode = (o.multi || o.expected_labels > 1 || o.rows * o.cols > 1) ? "multi" : "single";
        co.expected_count = o.expected_labels;
        co.expected_rows = o.rows;
        co.expected_cols = o.cols;
        co.max_skew_deg = o.max_skew_deg > 0 ? o.max_skew_deg : 4.0;
        co.systematic_ratio = o.systematic_ratio > 0 ? o.systematic_ratio : 0.8;
        co.consensus = o.consensus != 0;
        co.text_compare = o.text_compare != 0;
        co.color_calibrated = o.color_calibrated != 0;
        co.seed = o.seed ? o.seed : 1;
        if (o.data_check_path && *o.data_check_path) co.data_check = data_opts(o.data_check_path, o.data_check_row);
        auto model = ref->model;
        // PDF-prov: profilen pdf_vs_pdf om referensen öppnades med standardprofilen (som JobSpec)
        if (src.kind == compare::SampleSource::Kind::Pdf && model->opt.profile == "label_standard") {
            auto ro = model->opt;
            ro.profile = "pdf_vs_pdf";
            model = compare::analyse_reference(model->original, model->path, ro);
        }
        auto job = new fdc_job();
        job->engine = e;
        job->fn = fn;
        job->user = user;
        job->run([model, src, co](const JobContext& ctx) {
            auto r = std::make_unique<fdc_result>();
            r->kind = FDC_RESULT_LABEL;
            r->label = compare::compare_label(*model, src, co, ctx);
            r->j = r->label.result;
            return r;
        });
        *out = job;
        return FDC_OK;
    });
}

FDC_API fdc_status fdc_job_start_documents(fdc_engine* e, const char* ref_pdf, const char* smp_pdf, const fdc_document_options* opt,
                                        fdc_event_fn fn, void* user, fdc_job** out) {
    return guard([&] {
        if (!e || !ref_pdf || !smp_pdf || !out) return set_error(FDC_ERR_INVALID_ARGUMENT, "motor, filer eller out saknas");
        fdc_document_options d;
        fdc_document_options_init(&d);
        const fdc_document_options& o = opt ? *opt : d;
        compare::DocumentOptions dop;
        dop.layout = o.fixed_layout ? "fixed" : "reflow";
        dop.furniture_ignore = !o.compare_furniture;
        dop.typography_normalize = !o.strict_typography;
        dop.ignore_case = o.ignore_case != 0;
        dop.min_move_words = o.min_move_words > 0 ? o.min_move_words : 3;
        dop.move_tol_mm = o.move_tol_mm > 0 ? o.move_tol_mm : 0.3;
        if (o.data_check_path && *o.data_check_path) dop.data_check = data_opts(o.data_check_path, nullptr);
        std::string a = ref_pdf, b = smp_pdf;
        auto job = new fdc_job();
        job->engine = e;
        job->fn = fn;
        job->user = user;
        job->run([a, b, dop](const JobContext& ctx) {
            auto r = std::make_unique<fdc_result>();
            r->kind = FDC_RESULT_DOCUMENT;
            r->j = compare::run_document_job(a, b, dop, ctx);
            return r;
        });
        *out = job;
        return FDC_OK;
    });
}

FDC_API fdc_status fdc_job_start_data_check(fdc_engine* e, const char* pdf_path, const char* expected, const char* row, fdc_event_fn fn,
                                         void* user, fdc_job** out) {
    return guard([&] {
        if (!e || !pdf_path || !expected || !out) return set_error(FDC_ERR_INVALID_ARGUMENT, "motor, filer eller out saknas");
        std::string p = pdf_path;
        auto dop = data_opts(expected, row);
        auto job = new fdc_job();
        job->engine = e;
        job->fn = fn;
        job->user = user;
        job->run([p, dop](const JobContext& ctx) {
            ctx.progress(Stage::DataCheck, 0.0);
            doc::LoadOptions lo;
            lo.furniture = false;
            auto d = doc::load_file(p, lo);
            auto r = std::make_unique<fdc_result>();
            r->kind = FDC_RESULT_DATA_CHECK;
            r->j = compare::run_data_check(dop, &d, {});
            r->j["checked"] = p;
            return r;
        });
        *out = job;
        return FDC_OK;
    });
}

FDC_API fdc_status fdc_job_poll(fdc_job* job, fdc_event* out, int timeout_ms) {
    if (!job || !out) return set_error(FDC_ERR_INVALID_ARGUMENT, "jobb eller out saknas");
    std::unique_lock<std::mutex> l(job->m);
    auto ready = [&] { return !job->queue.empty(); };
    if (timeout_ms < 0) job->cv.wait(l, ready);
    else if (!job->cv.wait_for(l, std::chrono::milliseconds(timeout_ms), ready)) return FDC_ERR_TIMEOUT;
    job->last_polled = job->queue.front();
    job->queue.pop_front();
    uint32_t n = std::min<uint32_t>(out->size ? out->size : (uint32_t)sizeof(fdc_event), (uint32_t)sizeof(fdc_event));
    std::memcpy(out, &job->last_polled->ev, n);
    out->size = n;
    return FDC_OK;
}

FDC_API fdc_status fdc_job_cancel(fdc_job* job) {
    if (!job) return set_error(FDC_ERR_INVALID_ARGUMENT, "jobb saknas");
    job->cancel.cancel();
    return FDC_OK;
}

FDC_API fdc_status fdc_job_wait(fdc_job* job, int timeout_ms, fdc_result** out) {
    if (!job) return set_error(FDC_ERR_INVALID_ARGUMENT, "jobb saknas");
    std::unique_lock<std::mutex> l(job->m);
    auto ready = [&] { return job->done; };
    if (timeout_ms < 0) job->cv.wait(l, ready);
    else if (!job->cv.wait_for(l, std::chrono::milliseconds(timeout_ms), ready)) return FDC_ERR_TIMEOUT;
    if (job->status != FDC_OK) return set_error(job->status, job->error);
    if (out) {
        if (!job->result) return set_error(FDC_ERR_NOT_FOUND, "resultatet är redan hämtat");
        *out = job->result.release();
    }
    return FDC_OK;
}

FDC_API void fdc_job_free(fdc_job* job) {
    if (!job) return;
    job->cancel.cancel();
    if (job->worker.joinable()) job->worker.join();
    delete job;
}

FDC_API fdc_status fdc_compare(fdc_engine* e, fdc_reference* ref, const fdc_source* sample, const fdc_compare_options* opt, fdc_result** out) {
    fdc_job* job = nullptr;
    fdc_status s = fdc_job_start_compare(e, ref, sample, opt, nullptr, nullptr, &job);
    if (s != FDC_OK) return s;
    // utan återanrop köas händelserna; töm dem inte – vänta bara på resultatet
    s = fdc_job_wait(job, -1, out);
    std::string err = g_last_error;
    fdc_job_free(job);
    g_last_error = err;
    return s;
}


}  // extern "C"
