#include "mouse_win32.hpp"
#include "status_layout.hpp"

#include <array>
#include <cstring>
#include <string_view>
#include <utility>

namespace GOTHIC_NAMESPACE {
namespace {

using CharStats::Cell;
using CharStats::Pane;

constexpr unsigned long TextInk = 0xFFF0DFBB;
constexpr unsigned long ValueInk = 0xFFFFEAC4;
constexpr unsigned long BronzeInk = 0xFFDDB67D;
constexpr unsigned long LineInk = 0xFF655740;
constexpr unsigned short StatusLogicalKey = 0x14; // Used by the native status menu.

struct Box {
    int left = 0, top = 0, right = 0, bottom = 0;
    int Width() const { return right - left; }
    int Height() const { return bottom - top; }
    bool Contains(POINT p) const {
        return left <= p.x && p.x < right && top <= p.y && p.y < bottom;
    }
    bool operator==(const Box&) const = default;
};

bool IsOdysseePageOption(const zSTRING& section, const zSTRING& key) {
    return !_stricmp(section.ToChar(), "MOD_ODYSSEE") && !_stricmp(key.ToChar(), "showMoreInfo");
}

zCMenuItemChoice* FindOdysseePager(zCMenu& menu) {
    for (int i = 0; i < menu.m_listItems.GetNumInList(); ++i) {
        auto* item = menu.m_listItems[i];
        if (item && item->GetType() == MENU_ITEM_CHOICEBOX
            && !std::strcmp(item->GetName().ToChar(), "MENU_ITEM_SWITCH_HANDICRAFT_CHOICE")
            && IsOdysseePageOption(item->m_parOnChgSetOptionSection, item->m_parOnChgSetOption)) {
            return static_cast<zCMenuItemChoice*>(item);
        }
    }
    return nullptr;
}

std::optional<std::vector<Cell>> Capture(zCMenu& menu, const zCMenuItemChoice* pager, int page) {
    std::vector<Cell> cells;
    bool hasPageRows = false;
    for (int i = 0; i < menu.m_listItems.GetNumInList(); ++i) {
        auto* item = menu.m_listItems[i];
        if (!item) continue;
        const std::string id = item->GetName().ToChar();
        if (item == pager) {
            cells.push_back({item->m_parPosX, item->m_parPosY, id, {}, false, true});
            continue;
        }
        if (pager && id == "MENU_ITEM_SWITCH_HANDICRAFT") continue;

        const bool nativeVisible = item->owner && !item->m_bDontRender && item->GetIsVisible();
        const bool usesPageOption = pager &&
            IsOdysseePageOption(item->m_parHideIfOptionSectionSet, item->m_parHideIfOptionSet);
        const int hideOnPage = usesPageOption ? item->m_parHideOnValue : -1;
        if (!CharStats::VisibleOnPage(nativeVisible, hideOnPage, page)) continue;
        if (item->GetType() != MENU_ITEM_TEXT || (item->m_parItemFlags & IT_MULTILINE)) return std::nullopt;
        std::string text = item->GetText(0).ToChar();
        if (text.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        if (text.find_first_of("\r\n") != std::string::npos) return std::nullopt;
        cells.push_back({item->m_parPosX, item->m_parPosY, id, std::move(text),
                         id.ends_with("_HEADING"), false});
        hasPageRows |= hideOnPage == 0 || hideOnPage == 1;
    }
    if (pager && !hasPageRows) return std::nullopt;
    return cells;
}

void InitSprite(zCView& view, const char* name) {
    view.InsertBack(zSTRING(name));
    view.SetAlphaBlendFunc(zRND_ALPHA_FUNC_BLEND);
    view.SetTransparency(255);
}

void Place(zCView& view, Box box) {
    view.pposx = box.left;
    view.pposy = box.top;
    view.psizex = box.Width();
    view.psizey = box.Height();
}

zCOLOR Ink(unsigned long value) {
    zCOLOR color;
    color.dword = value;
    return color;
}

enum class HintStat { Strength, Dexterity, OneHand, TwoHand, Armor, Diving };

std::optional<HintStat> StatForHint(std::string_view id, bool odyssee) {
    if (id == "MENU_ITEM_ATTRIBUTE_1_TITLE") return HintStat::Strength;
    if (id == "MENU_ITEM_ATTRIBUTE_2_TITLE") return HintStat::Dexterity;
    if (id == "MENU_ITEM_TALENT_1_TITLE") return HintStat::OneHand;
    if (id == "MENU_ITEM_TALENT_2_TITLE") return HintStat::TwoHand;
    if (id == "MENU_ITEM_ARMOR_1_TITLE") return HintStat::Armor;
    if (id == "MENU_ITEM_ATTRIBUTE_8_TITLE" && odyssee) return HintStat::Diving;
    return std::nullopt;
}

std::optional<int> ScriptInt(const char* name) {
    auto* symbol = parser ? parser->GetSymbol(zSTRING(name)) : nullptr;
    if (!symbol || symbol->type != zPAR_TYPE_INT || !symbol->ele) return std::nullopt;
    int value = 0;
    symbol->GetValue(value, 0);
    return value;
}

void RestoreTrainingColumns(std::vector<Cell>& cells, const oCNpc& hero) {
    // Odyssee's script-driven training cells may still be empty during the capture.
    constexpr std::array totals{"MENU_ITEM_ATTRIBUTE_1", "MENU_ITEM_ATTRIBUTE_2", "MENU_ITEM_ATTRIBUTE_3"};
    for (std::size_t i = 0; i < totals.size(); ++i) {
        const std::string id = std::string(totals[i]) + "_SKILL";
        const std::string learned = std::to_string(hero.aiscriptvars[81 + i]);
        const auto native = std::ranges::find(cells, id, &Cell::id);
        if (native != cells.end()) {
            native->text = learned;
        } else if (const auto total = std::ranges::find(cells, totals[i], &Cell::id); total != cells.end()) {
            cells.push_back({total->x - 500, total->y, id, learned, false, false});
        }
    }
}

int LessonRate(int learned) {
    constexpr std::array thresholds{30, 60, 90, 120};
    return 1 + static_cast<int>(std::ranges::upper_bound(thresholds, learned) - thresholds.begin());
}

std::string MenuText(bool russian, const wchar_t* ru, const char* en) {
    if (!russian) return en;
    const int size = WideCharToMultiByte(1251, 0, ru, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return en;
    std::string text(size, '\0');
    if (!WideCharToMultiByte(1251, 0, ru, -1, text.data(), size, nullptr, nullptr)) return en;
    text.pop_back();
    return text;
}

std::vector<std::string> HintLines(HintStat stat, oCNpc& hero, bool russian, bool odyssee,
                                   bool standardTraining) {
    std::vector<std::string> lines;
    const auto label = [russian](const wchar_t* ru, const char* en) { return MenuText(russian, ru, en); };
    const auto add = [&](const wchar_t* ru, const char* en, int value, std::string suffix = {}) {
        lines.push_back(label(ru, en) + "  " + std::to_string(value) + suffix);
    };
    const auto lp = label(L" ОО", " LP");
    const auto addTraining = [&](int learned, int total) {
        const int rate = LessonRate(learned);
        if (rate < 5) {
            add(L"До подорожания", "Until the price increases", rate * 30 - learned);
            lines.push_back(label(L"Цена за +1  ", "Cost per +1  ") + std::to_string(rate)
                            + " -> " + std::to_string(rate + 1) + lp);
        } else {
            lines.push_back(label(L"Достигнут последний порог", "Highest price tier reached"));
            add(L"Цена за +1", "Cost per +1", rate, lp);
        }
        lines.push_back(label(L"Изучено  ", "Trained  ") + std::to_string(learned)
                        + label(L"   Всего  ", "   Total  ") + std::to_string(total));
    };

    switch (stat) {
    case HintStat::Strength:
        if (standardTraining) {
            addTraining(hero.aiscriptvars[81], hero.GetAttribute(NPC_ATR_STRENGTH));
        } else if (odyssee) {
            lines.push_back(label(L"Обычный арбалет  +", "Standard crossbow  +")
                + std::to_string(hero.GetAttribute(NPC_ATR_STRENGTH))
                + label(L" урона до защиты", " damage before armor"));
        } else {
            add(L"Полный удар (штатно)", "Full hit (stock)",
                hero.GetAttribute(NPC_ATR_STRENGTH), label(L" урона от силы", " damage from strength"));
        }
        break;
    case HintStat::Dexterity: {
        const int dex = hero.GetAttribute(NPC_ATR_DEXTERITY);
        if (standardTraining) {
            addTraining(hero.aiscriptvars[82], dex);
            break;
        }
        add(odyssee ? L"Поломка отмычки при ошибке" : L"Поломка отмычки (штатно)",
            odyssee ? "Lockpick breaks on error" : "Lockpick break (stock)",
            100 - std::clamp(dex, 0, 100), "%");
        if (odyssee) {
            constexpr std::array thresholds{60, 130, 200, 250};
            for (int i = 3; i >= 0; --i) {
                const std::string flag = "TALENT_SCHLOESSERKNACKEN" + std::to_string(i + 2);
                if (ScriptInt(flag.c_str()) != 1) continue;
                const int missing = thresholds[i] - dex;
                lines.push_back(label(L"Замки ", "Locks ") + std::to_string(i + 2) +
                    (missing <= 0 ? label(L" уровня  ловкости хватает", "  dexterity met") :
                        label(L" уровня  еще ", "  need ") + std::to_string(missing) +
                        label(L" ловкости", " dexterity")));
                break;
            }
        }
        break;
    }
    case HintStat::OneHand:
    case HintStat::TwoHand: {
        const bool oneHand = stat == HintStat::OneHand;
        const int skill = std::clamp(hero.GetHitChance(oneHand ? NPC_HITCHANCE_1H : NPC_HITCHANCE_2H), 0, 100);
        add(odyssee ? L"Шанс полного удара" : L"Полный удар (штатно)",
            odyssee ? "Full hit chance" : "Full hit (stock)", skill, "%");
        if (odyssee && skill < 60) {
            const int remaining = (skill < 30 ? 30 : 60) - skill;
            add(skill < 30 ? L"До бойца" : L"До мастера", skill < 30 ? "To Fighter" : "To Master",
                remaining, label(L" п. п.", " pp"));
            if (standardTraining) {
                const int learned = hero.aiscriptvars[oneHand ? 84 : 85];
                int cost = 0;
                for (int i = 0; i < remaining; ++i) cost += LessonRate(learned + i);
                add(L"Цена при уроках +1", "Cost with +1 lessons", cost, lp);
            }
        }
        break;
    }
    case HintStat::Armor:
        add(L"От рубящего", "Edge protection", hero.GetProtectionByIndex(oEDamageIndex_Edge));
        add(L"От дробящего", "Blunt protection", hero.GetProtectionByIndex(oEDamageIndex_Blunt));
        break;
    case HintStat::Diving: {
        float swim = 0.0f, capacity = 0.0f, remaining = 0.0f;
        hero.GetSwimDiveTime(swim, capacity, remaining);
        if (capacity >= 1000.0f && capacity <= 3600000.0f) {
            add(L"Запас воздуха", "Air capacity", static_cast<int>(capacity / 1000.0f),
                label(L" сек.", " sec."));
        }
        break;
    }
    }
    return lines;
}

class StatsPanel : public zCView {
    zCView solid{0, 0, 8, 8};
    zCView cursor{0, 0, 8, 8};
    zCView hintText{0, 0, 8, 8};
    std::array<std::vector<Cell>, 2> pages;
    int activePage = 0, maxLines = 0;
    bool paged = false, initialized = false;
    Box bounds;
    std::array<Box, 2> bodies{};
    std::array<int, 2> first{};
    std::array<int, 2> scrollLimit{};
    std::array<Box, 2> tracks{}, thumbs{};
    std::array<Box, 2> tabs{};
    std::vector<Box> sectionRules;
    Box closeButton;
    int divider = 0;
    int headerY = 0;
    bool dirty = true;
    bool hasCyrillicFont = false;
    bool odyssee = false, standardTraining = false;

    struct HintRow {
        Box bounds;
        HintStat stat;
        const Cell* title;
    };
    std::vector<HintRow> hintRows;
    const HintRow* hovered = nullptr;
    DWORD hoverStarted = 0;
    bool hintPrepared = false;
    Box hintBox;

    int Width(const std::string& text) {
        return GetFont()->GetFontX(zSTRING(text.c_str()));
    }

    void Text(int x, int y, const std::string& source, int limit,
              unsigned long ink, bool alignRight = false) {
        std::string text = source;
        if (hasCyrillicFont) {
            std::replace(text.begin(), text.end(), '\xA8', '\xC5');
            std::replace(text.begin(), text.end(), '\xB8', '\xE5');
        }
        if (Width(text) > limit) {
            const int dots = Width("...");
            if (dots >= limit) return;
            while (!text.empty() && Width(text) + dots > limit) text.pop_back();
            text += "...";
        }
        if (alignRight) x -= Width(text);
        const zSTRING line(text.c_str());
        auto color = Ink(ink);
        auto shadow = Ink(0xFF0A0907);
        CreateText(anx(x - bounds.left), any(y - bounds.top), line, 0.0f, color, FALSE, TRUE);
        CreateText(anx(x - bounds.left + 1), any(y - bounds.top + 1), line, 0.0f, shadow, FALSE, TRUE);
    }

    void PaintPane(const Pane& pane, int index, Box box, int line, int gutter) {
        const int visible = (std::max)(1, box.Height() / line);
        const int rows = static_cast<int>(pane.rows.size());
        scrollLimit[index] = (std::max)(0, rows - visible);
        first[index] = std::clamp(first[index], 0, scrollLimit[index]);
        if (scrollLimit[index] > 0) {
            const int thumb = (std::max)(10, box.Height() * visible / rows);
            const int y = box.top + (box.Height() - thumb) * first[index] / scrollLimit[index];
            tracks[index] = {box.right - 3, box.top, box.right, box.bottom};
            thumbs[index] = {box.right - 4, y, box.right + 1, y + thumb};
            box.right -= gutter;
        }

        std::vector<int> valueWidths;
        for (const auto& row : pane.rows) {
            if (row.size() <= 1) continue;
            valueWidths.resize((std::max)(valueWidths.size(), row.size() - 1));
            for (std::size_t j = 0; j + 1 < row.size(); ++j) {
                auto& width = valueWidths[j];
                width = (std::max)(width, Width(row[row.size() - 1 - j]->text));
            }
        }
        int totalWidth = 0;
        for (int width : valueWidths) totalWidth += width + gutter;
        const int available = (std::max)(1, box.Width() * 51 / 100);
        if (totalWidth > available) {
            for (int& width : valueWidths) width = (std::max)(1, width * available / totalWidth);
        }

        for (int i = first[index]; i < rows && i < first[index] + visible; ++i) {
            const auto& row = pane.rows[i];
            const int y = box.top + (i - first[index]) * line;
            if (row.front()->heading && i != first[index]) {
                sectionRules.push_back({box.left, y - 2, box.right, y - 1});
            }
            int valueX = box.right;
            for (std::size_t j = 0; j + 1 < row.size(); ++j) {
                Text(valueX, y, row[row.size() - 1 - j]->text, valueWidths[j], ValueInk, true);
                valueX -= valueWidths[j] + gutter;
            }
            if (const auto stat = StatForHint(row.front()->id, odyssee)) {
                hintRows.push_back({{box.left, y, box.right, (std::min)(box.bottom, y + line)},
                                    *stat, row.front()});
            }
            Text(box.left, y, row.front()->text, valueX - box.left,
                 row.front()->heading ? BronzeInk : TextInk);
        }
    }

    void Paint() {
        ClrPrintwin();
        ClearHover();
        hintRows.clear();
        bodies = {};
        tracks = {};
        thumbs = {};
        tabs = {};
        sectionRules.clear();
        scrollLimit = {};
        const auto panes = CharStats::MakePanes(pages[activePage]);
        const int margin = CharStats::PanelMargin(screen->psizey);
        const int line = CharStats::LineHeight(GetFont()->GetFontY());
        const int gutter = (std::max)(6, margin / 2);
        const bool split = panes[1].heading || panes[1].page || !panes[1].rows.empty();
        divider = split ? bounds.left + bounds.Width() * 43 / 100 : 0;

        const int closeRight = bounds.right - margin;
        headerY = bounds.top + margin + line + 2;
        closeButton = {closeRight - Width("Esc") - 2 * gutter, bounds.top + margin / 2,
                       closeRight + gutter, headerY};
        Text(closeRight, bounds.top + margin, "Esc", closeButton.Width(), BronzeInk, true);

        for (int i = 0; i < (split ? 2 : 1); ++i) {
            const auto& pane = panes[i];
            const int left = i == 0 ? bounds.left + margin : divider + margin;
            const int right = i == 0 && split ? divider - margin : bounds.right - margin;
            if (pane.page) {
                const std::array<std::string, 2> labels = hasCyrillicFont
                    ? std::array<std::string, 2>{"\xcd\xe0\xe2\xfb\xea\xe8", "\xd0\xe5\xec\xe5\xf1\xeb\xe0"}
                    : std::array<std::string, 2>{"Skills", "Crafts"};
                const int width = ((std::min)(right, closeButton.left) - left - 2 * gutter) / 2;
                for (int j = 0; j < 2; ++j) {
                    const int x = left + j * (width + gutter);
                    tabs[j] = {x, bounds.top + margin / 2, x + width, headerY};
                    const int labelX = x + (std::max)(0, (width - Width(labels[j])) / 2);
                    Text(labelX, bounds.top + margin, labels[j], width, j == activePage ? ValueInk : BronzeInk);
                }
            } else if (pane.heading) {
                Text(left, bounds.top + margin, pane.heading->text,
                     (std::min)(right, closeButton.left) - left - gutter, BronzeInk);
            }
            bodies[i] = {left, headerY + gutter, right, bounds.bottom - margin};
            PaintPane(pane, i, bodies[i], line, gutter);
        }
    }

    void DrawSolid(Box box, unsigned long ink) {
        if (box.Width() <= 0 || box.Height() <= 0 || !solid.backTex) return;
        Place(solid, box);
        solid.SetColor(Ink(ink));
        solid.Blit();
    }

    void ClearHover() {
        hovered = nullptr;
        hintPrepared = false;
        hintBox = {};
    }

    void BuildHint(const HintRow& row, oCNpc& hero) {
        const auto details = HintLines(row.stat, hero, hasCyrillicFont, odyssee, standardTraining);
        if (details.empty() || !solid.backTex) return;
        const int padding = (std::max)(8, CharStats::PanelMargin(screen->psizey) / 2);
        const int line = CharStats::LineHeight(GetFont()->GetFontY());
        const int maxWidth = (std::min)(screen->psizex - 2 * padding, bounds.Width());
        std::vector<std::string> lines{row.title->text};
        for (auto detail : details) {
            while (Width(detail) > maxWidth - 2 * padding) {
                auto cut = detail.find_last_of(' ');
                while (cut != std::string::npos && cut > 0
                       && Width(detail.substr(0, cut)) > maxWidth - 2 * padding) {
                    cut = detail.find_last_of(' ', cut - 1);
                }
                if (cut == std::string::npos || cut == 0) return;
                lines.push_back(detail.substr(0, cut));
                detail.erase(0, cut + 1);
            }
            lines.push_back(std::move(detail));
        }
        int width = 0;
        for (const auto& text : lines) width = (std::max)(width, Width(text));
        width += 2 * padding;
        const int height = 2 * padding + line * static_cast<int>(lines.size());
        if (width > maxWidth || height > screen->psizey - 2 * padding) return;
        hintBox = {0, 0, width, height};
        Place(hintText, hintBox);
        hintText.ClrPrintwin();
        int textY = padding;
        for (const auto& text : lines) {
            auto ink = Ink(textY == padding ? BronzeInk : TextInk);
            hintText.CreateText(hintText.anx(padding), hintText.any(textY), zSTRING(text.c_str()),
                                0.0f, ink, FALSE, TRUE);
            textY += line;
        }
    }

public:
    StatsPanel() : zCView(0, 0, 8192, 8192) {
        InitSprite(*this, "G2NRS_PANEL.TGA");
        SetTransparency(248);
        InitSprite(solid, "G2NRS_SOLID.TGA");
        InitSprite(cursor, "G2NRS_CURSOR.TGA");
    }

    bool Open(oCMenu_Status& menu, std::optional<int> rememberedPage) {
        if (!backTex || !screen || !screen->GetFont()) return false;
        auto* pager = FindOdysseePager(menu);
        auto a = Capture(menu, pager, 0);
        auto b = pager ? Capture(menu, pager, 1) : std::nullopt;
        if (!a || a->empty() || (pager && (!b || b->empty()))) return false;
        pages[0] = std::move(*a);
        if (b) pages[1] = std::move(*b);
        paged = pager != nullptr;
        odyssee = paged && ScriptInt("REAL_STRENGTH") == 81 && ScriptInt("REAL_DEXTERITY") == 82
            && ScriptInt("REAL_TALENT_1H") == 84 && ScriptInt("REAL_TALENT_2H") == 85
            && ScriptInt("TALENT_SCHLOESSERKNACKEN3").has_value();
        standardTraining = odyssee && ScriptInt("ODY_CHECKSNCMODE_ENABLE") == 0;
        if (standardTraining && oCNpc::player) {
            for (auto& page : pages) RestoreTrainingColumns(page, *oCNpc::player);
        }
        if (paged) {
            activePage = rememberedPage.value_or(pager->option == 1 ? 1 : 0);
        }
        auto* nativeFont = screen->GetFont();
        for (int i = 0; i < menu.m_listItems.GetNumInList(); ++i) {
            auto* item = menu.m_listItems[i];
            if (item && item->owner && item->GetIsVisible() && item->m_pFont) {
                nativeFont = item->m_pFont;
                break;
            }
        }
        SetFont(nativeFont);
        hintText.SetFont(nativeFont);
        hasCyrillicFont = std::ranges::any_of(pages[0], [](const Cell& cell) {
            return cell.heading && (std::count_if(cell.text.begin(), cell.text.end(),
                [](unsigned char c) { return c >= 0xC0; }) * 2 > static_cast<int>(cell.text.size()));
        });
        for (const auto& page : pages) {
            for (const auto& pane : CharStats::MakePanes(page)) {
                maxLines = (std::max)(maxLines, static_cast<int>(pane.rows.size()));
            }
        }
        initialized = true;
        return true;
    }

    bool Update() {
        if (!initialized || !screen || screen->psizex < 320 || screen->psizey < 240) return false;
        const auto [width, height] = CharStats::PanelSize(screen->psizex, screen->psizey,
                                                        GetFont()->GetFontY(), maxLines);
        const Box next{(screen->psizex - width) / 2, (screen->psizey - height) / 2,
                       (screen->psizex + width) / 2, (screen->psizey + height) / 2};
        if (next != bounds) {
            bounds = next;
            Place(*this, bounds);
            dirty = true;
        }
        if (std::exchange(dirty, false)) Paint();
        return true;
    }

    bool CloseAt(POINT p) const { return closeButton.Contains(p); }
    bool HasPages() const { return paged; }

    std::optional<int> PageAt(POINT p) const {
        for (int i = 0; i < 2; ++i) {
            if (tabs[i].Contains(p)) return i;
        }
        return std::nullopt;
    }

    void ShowPage(int page) {
        if (!paged || activePage == page) return;
        activePage = page;
        first[1] = 0;
        dirty = true;
    }

    void ScrollAt(POINT p, int steps) {
        for (int i = 0; i < 2; ++i) {
            if (!bodies[i].Contains(p)) continue;
            const int target = std::clamp(first[i] - 3 * std::clamp(steps, -8, 8), 0, scrollLimit[i]);
            dirty |= first[i] != target;
            first[i] = target;
        }
    }

    void Blit() override {
        const auto oldSource = zrenderer->GetAlphaBlendSource();
        zrenderer->SetAlphaBlendSource(zRND_ALPHA_SOURCE_MATERIAL);
        zCView::Blit();
        if (divider) DrawSolid({divider, headerY + 8, divider + 1, bodies[0].bottom}, LineInk);
        DrawSolid({bodies[0].left, headerY, (std::max)(bodies[0].right, bodies[1].right), headerY + 1}, LineInk);
        for (const auto& rule : sectionRules) DrawSolid(rule, LineInk);
        if (paged) {
            const auto& tab = tabs[activePage];
            DrawSolid({tab.left, tab.bottom - 1, tab.right, tab.bottom + 2}, BronzeInk);
        }
        for (int i = 0; i < 2; ++i) {
            DrawSolid(tracks[i], LineInk);
            DrawSolid(thumbs[i], BronzeInk);
        }
        zrenderer->SetAlphaBlendSource(oldSource);
    }

    void DrawFallbackCursor(POINT p) {
        if (!cursor.backTex) return;
        const int size = std::clamp(screen->psizey / 42, 16, 48);
        const auto oldSource = zrenderer->GetAlphaBlendSource();
        zrenderer->SetAlphaBlendSource(zRND_ALPHA_SOURCE_MATERIAL);
        Place(cursor, {p.x, p.y, p.x + size, p.y + size});
        cursor.Blit();
        zrenderer->SetAlphaBlendSource(oldSource);
    }

    void DrawHint(std::optional<POINT> position) {
        if (!position || !oCNpc::player) {
            ClearHover();
            return;
        }
        const auto hit = std::ranges::find_if(hintRows, [&](const HintRow& row) {
            return row.bounds.Contains(*position);
        });
        if (hit == hintRows.end()) {
            ClearHover();
            return;
        }
        const DWORD now = GetTickCount();
        if (&*hit != hovered) {
            ClearHover();
            hovered = &*hit;
            hoverStarted = now;
        }
        if (now - hoverStarted < 350) return;
        if (!std::exchange(hintPrepared, true)) BuildHint(*hovered, *oCNpc::player);
        if (hintBox.Width() <= 0) return;

        const int gap = std::clamp(screen->psizey / 64, 10, 24);
        const int width = hintBox.Width(), height = hintBox.Height();
        const int x = static_cast<int>(position->x), y = static_cast<int>(position->y);
        const int maxX = screen->psizex - width - 1, maxY = screen->psizey - height - 1;
        const int left = std::clamp(x + gap <= maxX ? x + gap : x - width - gap, 1, maxX);
        const int top = std::clamp(y + gap <= maxY ? y + gap : y - height - gap, 1, maxY);
        const Box card{left, top, left + width, top + height};
        Place(hintText, card);
        const auto oldSource = zrenderer->GetAlphaBlendSource();
        zrenderer->SetAlphaBlendSource(zRND_ALPHA_SOURCE_MATERIAL);
        const auto& row = hovered->bounds;
        DrawSolid({row.left - 5, row.top, row.left - 3, row.bottom}, BronzeInk);
        DrawSolid(card, LineInk);
        DrawSolid({card.left + 1, card.top + 1, card.right - 1, card.bottom - 1}, 0xFF202318);
        zrenderer->SetAlphaBlendSource(oldSource);
        hintText.DrawItems();
    }
};

void ApplyMouseAcquisition() {
    // The report-version engine applies the ZenGin mouse flag to DirectInput here.
    reinterpret_cast<int(__cdecl*)()>(0x004D3D30)();
}

std::optional<int> odysseePage;

class StatusSession {
    oCMenu_Status& primary;
    StatsPanel panel;
    HWND window = StatsMouse::WindowRoot(hWndApp);
    StatsMouse::MouseEvents input;
    StatsMouse::StatsPointer cursor;
    RECT client{};
    bool opened = false, ready = false, closing = false, listening = false, cursorActive = false;
    bool restoreMouse = false, centerOnOpen = true, leftDown = false, rightDown = false;

    void ReleaseMouse() {
        if (!std::exchange(cursorActive, false)) return;
        cursor.Leave();
        if (std::exchange(restoreMouse, false) && zinput
            && !zinput->GetDeviceEnabled(zINPUT_MOUSE)) {
            zinput->SetDeviceEnabled(zINPUT_MOUSE, TRUE);
            ApplyMouseAcquisition();
        }
    }

    std::optional<POINT> ScreenPoint(std::optional<POINT> point) const {
        if (!point || !screen || !PtInRect(&client, *point)) return std::nullopt;
        return POINT{MulDiv(point->x - client.left, screen->psizex, client.right - client.left),
                     MulDiv(point->y - client.top, screen->psizey, client.bottom - client.top)};
    }

    void SelectPage(int page) {
        odysseePage = page;
        panel.ShowPage(page);
    }

    void BeginClose() {
        if (closing) return;
        closing = true;
        input.Discard();
    }

    std::optional<POINT> ClickPoint(std::optional<POINT> event, int key, bool& down) const {
        const bool held = (GetAsyncKeyState(key) & 0x8000) != 0;
        const bool pressed = held && !down;
        down = held;
        if (event) return ScreenPoint(event);
        POINT desktop{};
        if (pressed && GetCursorPos(&desktop)) return ScreenPoint(desktop);
        return std::nullopt;
    }

public:
    explicit StatusSession(oCMenu_Status& menu) : primary(menu) {}

    ~StatusSession() {
        ReleaseMouse();
        input.Stop();
    }

    bool HandleKey(oCMenu_Status* menu, int key) {
        if (menu != &primary || !ready || zCMenu::GetActive() != menu) return false;
        if (key == KEY_ESCAPE || (zinput && zinput->GetFirstBindedLogicalKey(key) == StatusLogicalKey)) {
            BeginClose();
            return false;
        }
        if (closing) return true;
        if (!panel.HasPages()) return false;
        if (key == KEY_LEFT || key == KEY_RIGHT) {
            SelectPage(key == KEY_RIGHT ? 1 : 0);
            return true;
        }
        // The stock Odyssee item starts MENU_STATUS again on Enter.
        return key == KEY_RETURN || key == KEY_NUMPADENTER;
    }

    void BeforeFrame(zCMenu* menu) {
        if (menu != &primary || zCMenu::GetActive() != menu) return;
        if (input.TakeFocusLoss() || !StatsMouse::HasFocus(window)) {
            ReleaseMouse();
            input.Discard();
        }
        if (closing || !ready || !menu->m_screenInitDone || !zinput || !StatsMouse::HasFocus(window)
            || !StatsMouse::ClientBounds(window, client)) return;
        if (!cursorActive) {
            if (!listening) listening = input.Start(window);
            // Even if the Windows message hooks are unavailable, poll button edges.
            restoreMouse = zinput->GetDeviceEnabled(zINPUT_MOUSE) != 0;
            if (restoreMouse) {
                zinput->SetDeviceEnabled(zINPUT_MOUSE, FALSE);
                ApplyMouseAcquisition();
            }
            cursor.Enter(window, client, std::exchange(centerOnOpen, false));
            leftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            rightDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
            cursorActive = true;
        }
        if (menu->m_exitState != zCMenu::NONE) return;
        const auto events = input.Take();
        const auto click = ClickPoint(events.leftClick, VK_LBUTTON, leftDown);
        const auto right = ClickPoint(events.rightClick, VK_RBUTTON, rightDown);
        if (right || (click && panel.CloseAt(*click))) {
            BeginClose();
            menu->HandleEvent(KEY_ESCAPE);
            return;
        }
        if (click) {
            if (const auto page = panel.PageAt(*click)) SelectPage(*page);
        }
        if (const auto wheel = ScreenPoint(events.wheelPosition); wheel && events.wheel) {
            panel.ScrollAt(*wheel, events.wheel);
        }
    }

    bool ReplaceDraw(zCView* native) {
        if (native != primary.m_pWindow) return false;
        // ScreenDone can change the active menu while the old window still draws.
        if (ready && (closing || primary.m_exitState != zCMenu::NONE)) return true;
        if (zCMenu::GetActive() != &primary || !primary.m_screenInitDone) return false;
        if (!opened) {
            opened = true;
            ready = panel.Open(primary, odysseePage);
        }
        if (!ready || !panel.Update()) return false;
        panel.DrawItems();
        POINT desktop{};
        const auto point = cursorActive && StatsMouse::HasFocus(window) && GetCursorPos(&desktop)
            ? ScreenPoint(desktop) : std::nullopt;
        panel.DrawHint(point);
        if (point) {
            CURSORINFO info{};
            info.cbSize = sizeof(info);
            if (!GetCursorInfo(&info) || !(info.flags & CURSOR_SHOWING) || !info.hCursor) {
                panel.DrawFallbackCursor(*point);
            }
        }
        return true;
    }
};

StatusSession* current = nullptr;

void __fastcall ShowStatus(oCStatusScreen*, void*);
auto ShowHook = Union::CreateHook(
    SIGNATURE_OF(&oCStatusScreen::Show), &ShowStatus, Union::HookType::Hook_Detours);

void __fastcall ShowStatus(oCStatusScreen* self, void* edx) {
    if (!self->m_pStatusMenu || !screen || current) {
        ShowHook(self, edx);
        return;
    }
    StatusSession session(*self->m_pStatusMenu);
    current = &session;
    ShowHook(self, edx);
    current = nullptr;
}

void __fastcall MenuFrame(zCMenu*, void*, int);
auto FrameHook = Union::CreateHook(
    SIGNATURE_OF(&zCMenu::HandleFrame), &MenuFrame, Union::HookType::Hook_Detours);

void __fastcall MenuFrame(zCMenu* self, void* edx, int render) {
    if (current) current->BeforeFrame(self);
    FrameHook(self, edx, render);
}

int __fastcall StatusKey(oCMenu_Status*, void*, int);
auto KeyHook = Union::CreateHook(
    SIGNATURE_OF(&oCMenu_Status::HandleEvent), &StatusKey, Union::HookType::Hook_Detours);

int __fastcall StatusKey(oCMenu_Status* self, void* edx, int key) {
    if (current && current->HandleKey(self, key)) return TRUE;
    return KeyHook(self, edx, key);
}

void __fastcall DrawView(zCView*, void*);
auto DrawHook = Union::CreateHook(
    SIGNATURE_OF(&zCView::DrawItems), &DrawView, Union::HookType::Hook_Detours);

void __fastcall DrawView(zCView* self, void* edx) {
    if (!current || !current->ReplaceDraw(self)) DrawHook(self, edx);
}

} // namespace
} // namespace GOTHIC_NAMESPACE
