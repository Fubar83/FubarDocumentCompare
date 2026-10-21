// Privat för FubarDocumentCompare.Native.dll: handtagens innehåll och hjälp för C-gränssnittet.
#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <opencv2/imgproc.hpp>
#include <thread>

#include "printcmp/compare/compare.hpp"   // C-API:t är en fasad över hela pc_compare
#include "printcmp/core/text.hpp"
#include "printcmp/docview/docview.hpp"
#include "printcmp/image/image.hpp"
#include "fdc/fdc.h"
#include "printcmp/report/report.hpp"



namespace pc::capi {

extern thread_local std::string g_last_error;   // fdc_last_error() för den anropande tråden

fdc_status set_error(fdc_status s, const std::string& msg);
fdc_status from_code(ErrorCode c);
std::vector<std::string> split_list(const char* s);
const char* profile_name(fdc_profile p);
fdc_kind kind_of(const std::string& k);
fdc_severity sev_of(const std::string& s);
fdc_certainty cert_of(const std::string& s);
std::string str_of(const json& v);

// Kör f och översätter undantag till en felkod (och trådens feltext).
template <class F>
fdc_status guard(F&& f) {
    try {
        g_last_error.clear();
        return f();
    } catch (const Error& e) {
        return set_error(from_code(e.code()), e.what());
    } catch (const std::exception& e) {
        return set_error(FDC_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_error(FDC_ERR_INTERNAL, "okänt fel");
    }
}

}  // namespace pc::capi

using namespace pc;
using namespace pc::capi;

// ------------------------------------------------------------------ objekt

struct fdc_engine {
    int max_jobs = 1;
    std::unique_ptr<docview::Cache> cache;   // PDF-tjänsterna: innehållsadresserad, bara för hastighet
    std::mutex m;
    std::condition_variable cv;
    int running = 0;
    void acquire() {
        std::unique_lock<std::mutex> l(m);
        cv.wait(l, [&] { return running < max_jobs; });
        ++running;
    }
    void release() {
        {
            std::lock_guard<std::mutex> l(m);
            --running;
        }
        cv.notify_one();
    }
};

struct fdc_reference {
    std::shared_ptr<const compare::ReferenceModel> model;
    std::vector<std::string> layer_names, spot_names, code_data, code_sym, braille_text;
};

struct FindingStrings {
    std::string category, from, to, detail, reasons, fingerprint;
    bool has_from = false, has_to = false;
};

struct DataStrings {
    std::string field, expected, found, diff;
};

struct fdc_result {
    fdc_result_kind kind = FDC_RESULT_LABEL;
    json j;
    compare::LabelResult label;          // bilder för utsnitt och rapport (etikettläge)
    std::vector<FindingStrings> fs;
    std::vector<DataStrings> ds;
    const json& findings() const {
        static const json empty = json::array();
        if (kind == FDC_RESULT_LABEL) return j["differences"];
        if (kind == FDC_RESULT_DOCUMENT) return j["findings"];
        return empty;
    }
    const json* data_check() const {
        if (kind == FDC_RESULT_DATA_CHECK) return &j;
        return j.contains("data_check") ? &j["data_check"] : nullptr;
    }
    void prepare();
};

struct OwnedEvent {
    fdc_event ev{};
    std::string code, message;
};

struct fdc_job {
    fdc_engine* engine = nullptr;
    fdc_event_fn fn = nullptr;
    void* user = nullptr;
    CancelToken cancel;
    std::thread worker;
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::shared_ptr<OwnedEvent>> queue;
    std::shared_ptr<OwnedEvent> last_polled;
    bool done = false;
    fdc_status status = FDC_OK;
    std::string error;
    std::unique_ptr<fdc_result> result;

    void push(std::shared_ptr<OwnedEvent> e) {
        e->ev.code = e->code.empty() ? nullptr : e->code.c_str();
        e->ev.message = e->message.empty() ? nullptr : e->message.c_str();
        if (fn) {
            fn(this, &e->ev, user);
            return;
        }
        std::lock_guard<std::mutex> l(m);
        queue.push_back(std::move(e));
        cv.notify_all();
    }
    void emit(const Event& src) {
        auto e = std::make_shared<OwnedEvent>();
        auto& ev = e->ev;
        ev.size = sizeof(fdc_event);
        ev.stage = (fdc_stage)src.stage;
        ev.fraction = src.fraction;
        ev.index = src.index;
        ev.rotation_deg = src.rotation_deg;
        for (int k = 0; k < 4; ++k) ev.corners[k] = {src.corners[k].x, src.corners[k].y};
        ev.width_px = src.width_px;
        ev.height_px = src.height_px;
        ev.dpi = src.dpi;
        ev.preliminary = src.preliminary;
        ev.label_status = (fdc_label_status)src.status;
        switch (src.type) {
            case Event::Type::Progress: ev.type = FDC_EVENT_PROGRESS; break;
            case Event::Type::SheetLoaded: ev.type = FDC_EVENT_SHEET_LOADED; break;
            case Event::Type::LabelLocated: ev.type = FDC_EVENT_LABEL_LOCATED; break;
            case Event::Type::FoldDetected: ev.type = FDC_EVENT_FOLD_DETECTED; break;
            case Event::Type::LabelAnalysed: ev.type = FDC_EVENT_LABEL_ANALYSED; break;
            case Event::Type::Warning: ev.type = FDC_EVENT_WARNING; break;
        }
        e->code = src.code;
        e->message = src.message;
        push(e);
    }
    void finish(fdc_status s, const std::string& msg, std::unique_ptr<fdc_result> r) {
        {
            std::lock_guard<std::mutex> l(m);
            status = s;
            error = msg;
            result = std::move(r);
            done = true;
        }
        auto e = std::make_shared<OwnedEvent>();
        e->ev.size = sizeof(fdc_event);
        e->ev.type = s == FDC_OK ? FDC_EVENT_COMPLETED : s == FDC_ERR_CANCELLED ? FDC_EVENT_CANCELLED : FDC_EVENT_FAILED;
        e->ev.stage = FDC_STAGE_DONE;
        e->ev.fraction = 1.0;
        e->ev.error = s;
        e->message = msg;
        push(e);
        cv.notify_all();
    }
    template <class F>
    void run(F&& body) {
        worker = std::thread([this, body = std::forward<F>(body)]() mutable {
            engine->acquire();
            JobContext ctx;
            ctx.cancel = &cancel;
            ctx.sink = [this](const Event& e) { emit(e); };
            try {
                auto r = body(ctx);
                r->prepare();
                engine->release();
                finish(FDC_OK, "", std::move(r));
            } catch (const Error& e) {
                engine->release();
                finish(from_code(e.code()), e.what(), nullptr);
            } catch (const std::exception& e) {
                engine->release();
                finish(FDC_ERR_INTERNAL, e.what(), nullptr);
            }
        });
    }
};


// ------------------------------------------------------------------ PDF-tjänster

struct fdc_buffer {
    std::vector<unsigned char> data;
};

struct fdc_pdf {
    docview::Pdf pdf;
};

struct fdc_image {
    pdf::Raster raster;
};

namespace pc::capi {

// Läs ett versionerat alternativ-struct: standardvärden, sedan så många byte som anroparen känner till.
template <class T>
T read_options(const T* o, void (*init)(T*)) {
    T d;
    init(&d);
    if (o) std::memcpy(&d, o, std::min<size_t>(o->size ? o->size : sizeof(T), sizeof(T)));
    d.size = sizeof(T);
    return d;
}

}  // namespace pc::capi

struct fdc_fontset {
    std::shared_ptr<const pdf::FontSet> set;
};
