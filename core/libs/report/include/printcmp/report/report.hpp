// pc_report: export – result.json, fristående HTML-rapporter och bildlager (inget annat).
#pragma once

#include <string>

#include "printcmp/compare/label.hpp"

namespace pc::report {

void write_json(const std::string& path, const json& result);
void write_label_html(const std::string& path, const compare::LabelResult& r);
void write_layers(const std::string& dir, const compare::LabelResult& r, double layer_dpi);
void write_document_html(const std::string& path, const json& result);
std::string data_check_html(const json& dc, const std::string& title = "Datakontroll");

}  // namespace pc::report
