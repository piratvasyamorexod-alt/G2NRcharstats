#pragma once

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace CharStats {

inline bool VisibleOnPage(bool nativeVisible, int hideOnPage, int page) {
    return hideOnPage == 0 || hideOnPage == 1 ? page != hideOnPage : nativeVisible;
}

inline int PanelMargin(int screenHeight) { return std::clamp(screenHeight / 46, 12, 29); }
inline int LineHeight(int fontHeight) { return (std::max)(12, fontHeight) + 4; }

inline std::pair<int, int> PanelSize(int screenWidth, int screenHeight, int fontHeight, int rows) {
    const int margin = PanelMargin(screenHeight);
    const int line = LineHeight(fontHeight);
    const int width = (std::min)(screenWidth * 92 / 100, screenHeight * 98 / 100);
    const int height = (std::min)(screenHeight * 70 / 100, 3 * margin + 2 + (rows + 1) * line);
    return {width, height};
}

struct Cell {
    int x, y;
    std::string id, text;
    bool heading, choice;

    bool operator==(const Cell&) const = default;
};

using Row = std::vector<const Cell*>;

struct Pane {
    std::vector<Row> rows;
    const Cell* heading = nullptr;
    const Cell* page = nullptr;
};

inline std::array<Pane, 2> MakePanes(const std::vector<Cell>& cells) {
    std::array<Pane, 2> panes;
    std::array<std::vector<const Cell*>, 2> columns;
    const auto right = std::find_if(cells.begin(), cells.end(), [](const Cell& cell) {
        return cell.id == "MENU_ITEM_TALENT_1_TITLE"
            || cell.id == "MENU_ITEM_TALENTS_HEADING"
            || cell.id == "MENU_ITEM_SWITCH_HANDICRAFT_CHOICE";
    });
    const auto left = std::find_if(cells.begin(), cells.end(), [](const Cell& cell) {
        return cell.id == "MENU_ITEM_ATTRIBUTE_1_TITLE";
    });
    const bool split = right != cells.end() && left != cells.end() && right->x > left->x;

    for (const auto& cell : cells) {
        const auto index = split && cell.x >= right->x ? 1 : 0;
        if (cell.choice) {
            panes[index].page = &cell;
        } else if (cell.id == "MENU_ITEM_STATUS_HEADING" || cell.id == "MENU_ITEM_TALENTS_HEADING") {
            panes[index].heading = &cell;
        } else {
            columns[index].push_back(&cell);
        }
    }
    for (std::size_t i = 0; i < columns.size(); ++i) {
        auto& cellsByPosition = columns[i];
        std::stable_sort(cellsByPosition.begin(), cellsByPosition.end(), [](const Cell* a, const Cell* b) {
            return a->y < b->y || (a->y == b->y && a->x < b->x);
        });
        auto& rows = panes[i].rows;
        for (const auto* cell : cellsByPosition) {
            // Group headings occupy their own line. A mod may slightly offset values.
            if (rows.empty() || cell->heading || rows.back().front()->heading
                || cell->y - rows.back().front()->y > 12) {
                rows.emplace_back();
            }
            rows.back().push_back(cell);
        }
        for (auto& row : rows) {
            std::stable_sort(row.begin(), row.end(), [](const Cell* a, const Cell* b) {
                return a->x < b->x;
            });
        }
    }
    return panes;
}

} // namespace CharStats
