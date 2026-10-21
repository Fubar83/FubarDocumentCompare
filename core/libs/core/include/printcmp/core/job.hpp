// Jobb: steg, händelser, avbrott och sammanhanget som skickas genom analysen.
#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "printcmp/core/error.hpp"
#include "printcmp/core/types.hpp"

namespace pc {

class CancelToken {
public:
    void cancel() { flag_.store(true); }
    bool cancelled() const { return flag_.load(); }
    void check() const {
        if (cancelled()) throw Error(ErrorCode::Cancelled, "avbrutet");
    }

private:
    std::atomic<bool> flag_{false};
};

enum class Stage : int { Render = 0, Locate, Register, Normalize, Diff, Folds, Classify, Aggregate, Text, DataCheck, Report, Done };
const char* stage_name(Stage s);

// Händelser under ett jobb (se include/printcmp/printcmp.h för C-motsvarigheten).
struct Event {
    enum class Type { Progress, SheetLoaded, LabelLocated, FoldDetected, LabelAnalysed, Warning } type;
    Stage stage = Stage::Render;
    double fraction = 0;
    int index = 0;                 // etikett (1-baserad)
    double rotation_deg = 0;
    Pt corners[4];                 // etikettens hörn i provets px (LabelLocated) / veckets ändar (FoldDetected)
    double width_px = 0, height_px = 0, dpi = 0;
    int preliminary = 0;           // LabelAnalysed: preliminärt antal fynd
    int status = 0;                // 0 = godkänd, 1 = varning, 2 = underkänd
    std::string code, message;     // Warning
};

using EventSink = std::function<void(const Event&)>;

struct JobContext {
    EventSink sink;
    const CancelToken* cancel = nullptr;
    void emit(const Event& e) const {
        if (sink) sink(e);
    }
    void progress(Stage s, double f) const {
        Event e{Event::Type::Progress};
        e.stage = s;
        e.fraction = f;
        emit(e);
    }
    void warn(const std::string& code, const std::string& msg) const {
        Event e{Event::Type::Warning};
        e.code = code;
        e.message = msg;
        emit(e);
    }
    void check() const {
        if (cancel) cancel->check();
    }
};

}  // namespace pc
