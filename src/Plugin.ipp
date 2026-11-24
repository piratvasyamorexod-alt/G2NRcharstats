#include "mouse_win32.hpp"
#include "status_layout.hpp"

#include <array>
#include <cstring>
#include <utility>

namespace GOTHIC_NAMESPACE {
namespace {

using CharStats::Cell;
using CharStats::Pane;

constexpr unsigned long TextInk = 0xFFE0CDA4;
constexpr unsigned long ValueInk = 0xFFF1DDB3;
constexpr unsigned long BronzeInk = 0xFFD6AC70;
constexpr unsigned long LineInk = 0xFF63533D;

struct Box {
    int left = 0, top = 0, right = 0, bottom = 0;
    int Width() const { return right - left; }
    int Height() const { return bottom - top; }
    bool Contains(POINT p) const {
        return left <= p.x && p.x < right && top <= p.y && p.y < bottom;
    }
    bool operator==(const Box&) const = default;
};

bool IsStatusMenu(zCMenu* menu) {
    return menu && !std::strcmp(menu->GetName().ToChar(), "MENU_STATUS");
}

std::optional<std::vector<Cell>> Capture(zCMenu& menu) {
    std::vector<Cell> cells;
    for (int i = 0; i < menu.m_listItems.GetNumInList(); ++i) {
        auto* item = menu.m_listItems[i];
        if (!item || !item->owner || item->m_bDontRender || !item->GetIsVisible()) continue;
        const auto type = item->GetType();
        const std::string id = item->GetName().ToChar();
        const bool choice = type == MENU_ITEM_CHOICEBOX && id == "MENU_ITEM_SWITCH_HANDICRAFT_CHOICE";
        if (type != MENU_ITEM_TEXT && !choice) return std::nullopt;
        if (item->m_parItemFlags & IT_MULTILINE) return std::nullopt;

        const zSTRING value = choice
            ? static_cast<zCMenuItemChoice*>(item)->GetStringByOption()
            : item->GetText(0);
        std::string text = value.ToChar();
        if (text.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        if (text.find_first_of("\r\n") != std::string::npos) return std::nullopt;
        cells.push_back({item->m_parPosX, item->m_parPosY, id, std::move(text),
                         id.ends_with("_HEADING"), choice});
    }
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

class StatsPanel : public zCView {
    zCView solid{0, 0, 8, 8};
    zCView cursor{0, 0, 8, 8};
    std::vector<Cell> lastCells;
    zCMenu* lastMenu = nullptr;
    Box bounds;
    std::array<Box, 2> bodies{};
    std::array<int, 2> first{};
    std::array<int, 2> scrollLimit{};
    std::array<Box, 2> tracks{}, thumbs{};
    Box closeButton, pageButton;
    int divider = 0;
    int headerY = 0;
    bool dirty = true;
    bool hasCyrillicFont = false;

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
            tracks[index] = {box.right - 4, box.top, box.right, box.bottom};
            thumbs[index] = {box.right - 5, y, box.right + 1, y + thumb};
            box.right -= gutter;
        }

        // Reserve enough space for the values, including a mod's extra columns.
        // Work from the right so a row with a single value shares the same edge.
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
        const int available = (std::max)(1, box.Width() * 54 / 100);
        if (totalWidth > available) {
            for (int& width : valueWidths) width = (std::max)(1, width * available / totalWidth);
        }

        for (int i = first[index]; i < rows && i < first[index] + visible; ++i) {
            const auto& row = pane.rows[i];
            const int y = box.top + (i - first[index]) * line;
            int valueX = box.right;
            for (std::size_t j = 0; j + 1 < row.size(); ++j) {
                Text(valueX, y, row[row.size() - 1 - j]->text, valueWidths[j], ValueInk, true);
                valueX -= valueWidths[j] + gutter;
            }
            Text(box.left, y, row.front()->text, valueX - box.left,
                 row.front()->heading ? BronzeInk : TextInk);
        }
    }

    void Paint() {
        ClrPrintwin();
        bodies = {};
        tracks = {};
        thumbs = {};
        pageButton = {};
        scrollLimit = {};
        const auto panes = CharStats::MakePanes(lastCells);
        const int margin = std::clamp(bounds.Height() / 30, 14, 30);
        const int glyph = (std::max)(12, GetFont()->GetFontY());
        const int line = glyph + std::clamp(bounds.Height() / 175, 3, 6);
        const int gutter = (std::max)(6, margin / 2);
        const bool split = panes[1].heading || panes[1].page || !panes[1].rows.empty();
        divider = split ? bounds.left + bounds.Width() * 43 / 100 : 0;

        const int closeRight = bounds.right - margin;
        headerY = bounds.top + margin + line + gutter;
        closeButton = {closeRight - Width("Esc") - margin, bounds.top + margin / 2,
                       closeRight + gutter, headerY};
        Text(closeRight, bounds.top + margin, "Esc", closeButton.Width(), BronzeInk, true);

        for (int i = 0; i < (split ? 2 : 1); ++i) {
            const auto& pane = panes[i];
            const int left = i == 0 ? bounds.left + margin : divider + margin;
            const int right = i == 0 && split ? divider - margin : bounds.right - margin;
            if (const auto* header = pane.page ? pane.page : pane.heading) {
                const int end = (std::min)(right, closeButton.left) - gutter;
                Text(left, bounds.top + margin, header->text, end - left, BronzeInk);
                if (pane.page) {
                    pageButton = {left - gutter, bounds.top + margin / 2,
                                  (std::min)(left + Width(header->text) + gutter, end), headerY};
                }
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

public:
    StatsPanel() : zCView(0, 0, 8192, 8192) {
        InitSprite(*this, "G2NRS_PANEL.TGA");
        SetTransparency(213);
        InitSprite(solid, "G2NRS_SOLID.TGA");
        InitSprite(cursor, "G2NRS_CURSOR.TGA");
    }

    bool Update(zCMenu& menu) {
        if (!backTex || !screen || !screen->GetFont() || screen->psizex < 320 || screen->psizey < 240) return false;
        auto cells = Capture(menu);
        if (!cells || cells->empty()) return false;
        const auto width = (std::min)(screen->psizex * 84 / 100, screen->psizey * 159 / 100);
        const auto height = screen->psizey * 84 / 100;
        const Box next{(screen->psizex - width) / 2, (screen->psizey - height) / 2,
                       (screen->psizex + width) / 2, (screen->psizey + height) / 2};
        if (&menu != lastMenu || *cells != lastCells) {
            first = {};
            lastCells = std::move(*cells);
            lastMenu = &menu;
            dirty = true;
        }
        auto* nativeFont = screen->GetFont();
        for (int i = 0; i < menu.m_listItems.GetNumInList(); ++i) {
            auto* item = menu.m_listItems[i];
            if (item && item->owner && item->GetIsVisible() && item->m_pFont) {
                nativeFont = item->m_pFont;
                break;
            }
        }
        if (next != bounds || GetFont() != nativeFont) {
            bounds = next;
            Place(*this, bounds);
            SetFont(nativeFont);
            // Check a native heading rather than the user's Windows locale.
            hasCyrillicFont = std::ranges::any_of(lastCells, [](const Cell& cell) {
                return cell.heading && (std::count_if(cell.text.begin(), cell.text.end(),
                    [](unsigned char c) { return c >= 0xC0; }) * 2 > static_cast<int>(cell.text.size()));
            });
            dirty = true;
        }
        if (std::exchange(dirty, false)) Paint();
        return true;
    }

    bool CloseAt(POINT p) const { return closeButton.Contains(p); }

    bool PageAt(POINT p) const { return pageButton.Contains(p); }

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
        if (divider) DrawSolid({divider, headerY + 8, divider + 1, bounds.bottom - 20}, LineInk);
        DrawSolid({bounds.left + 20, headerY, bounds.right - 20, headerY + 1}, LineInk);
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
};

void ApplyMouseAcquisition() {
    // The report-version engine applies the ZenGin mouse flag to DirectInput here.
    reinterpret_cast<int(__cdecl*)()>(0x004D3D30)();
}

class StatusSession {
    oCMenu_Status& primary;
    StatsPanel panel;
    HWND window = StatsMouse::WindowRoot(hWndApp);
    StatsMouse::MouseEvents input;
    StatsMouse::StatsPointer cursor;
    RECT client{};
    bool ready = false, listening = false, cursorActive = false;
    bool restoreMouse = false, centerOnOpen = true;

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

public:
    explicit StatusSession(oCMenu_Status& menu) : primary(menu) {}

    ~StatusSession() {
        ReleaseMouse();
        input.Stop();
    }

    void BeforeFrame(zCMenu* menu) {
        if (menu != zCMenu::GetActive()) return;
        const bool ours = menu == &primary || IsStatusMenu(menu);
        if (input.TakeFocusLoss() || !ours || !StatsMouse::HasFocus(window)) {
            ReleaseMouse();
            input.Discard();
        }
        if (!ours || !ready || !menu->m_screenInitDone || !zinput || !StatsMouse::HasFocus(window)
            || !StatsMouse::ClientBounds(window, client)) return;
        if (!cursorActive) {
            if (!listening) listening = input.Start(window);
            if (!listening) return;
            restoreMouse = zinput->GetDeviceEnabled(zINPUT_MOUSE) != 0;
            if (restoreMouse) {
                zinput->SetDeviceEnabled(zINPUT_MOUSE, FALSE);
                ApplyMouseAcquisition();
            }
            cursor.Enter(window, client, std::exchange(centerOnOpen, false));
            cursorActive = true;
        }
        if (menu->m_exitState != zCMenu::NONE) return;
        const auto events = input.Take();
        const auto click = ScreenPoint(events.leftClick);
        if (ScreenPoint(events.rightClick) || (click && panel.CloseAt(*click))) {
            menu->HandleEvent(KEY_ESCAPE);
            return;
        }
        if (click && panel.PageAt(*click)) {
            menu->HandleEvent(KEY_RIGHT);
            menu->HandleEvent(KEY_RETURN);
            return;
        }
        if (const auto wheel = ScreenPoint(events.wheelPosition); wheel && events.wheel) {
            panel.ScrollAt(*wheel, events.wheel);
        }
    }

    bool ReplaceDraw(zCView* native) {
        auto* menu = zCMenu::GetActive();
        if (!menu || (menu != &primary && !IsStatusMenu(menu)) || !menu->m_screenInitDone) return false;
        if (native != menu->m_pWindow) {
            // A native page change can run a fresh MENU_STATUS over the previous one.
            return menu != &primary && native == primary.m_pWindow && ready;
        }
        ready = panel.Update(*menu);
        if (!ready) {
            ReleaseMouse();
            return false;
        }
        panel.DrawItems();
        if (cursorActive && StatsMouse::HasFocus(window)) {
            CURSORINFO info{};
            info.cbSize = sizeof(info);
            POINT desktop{};
            if ((!GetCursorInfo(&info) || !(info.flags & CURSOR_SHOWING) || !info.hCursor)
                && GetCursorPos(&desktop)) {
                if (const auto p = ScreenPoint(desktop)) panel.DrawFallbackCursor(*p);
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

void __fastcall DrawView(zCView*, void*);
auto DrawHook = Union::CreateHook(
    SIGNATURE_OF(&zCView::DrawItems), &DrawView, Union::HookType::Hook_Detours);

void __fastcall DrawView(zCView* self, void* edx) {
    if (!current || !current->ReplaceDraw(self)) DrawHook(self, edx);
}

} // namespace
} // namespace GOTHIC_NAMESPACE
