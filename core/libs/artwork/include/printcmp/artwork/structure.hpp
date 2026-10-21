// Tabeller (med rutnät, med vågräta linjer eller utan linjer) och var en ruta ligger i dem.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "printcmp/artwork/objmap.hpp"

namespace pc::artwork {

struct TableRule {
    Box bbox;
    char orient;   // 'h' | 'v'
    int obj = 0;
    int position = 0;
};

struct Table {
    int id = 0;
    std::string kind;                  // ruled_grid | ruled_horizontal | unruled
    Box bbox;
    std::vector<double> row_bounds, col_bounds;
    std::map<std::pair<int, int>, std::string> cells;
    std::vector<TableRule> rules;
    int n_rows() const { return (int)row_bounds.size() - 1; }
    int n_cols() const { return (int)col_bounds.size() - 1; }
    json to_json(double ppm) const;
};

std::vector<Table> detect_tables(const std::vector<Obj>& objs, double ppm);
std::optional<json> locate_in_table(const std::vector<Table>& tables, const Box& box, double ppm);

}  // namespace pc::artwork
