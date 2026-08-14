#include "InputDialog.h"
#include "quickyeet/HistoryStore.h"
#include "quickyeet/MoveEngine.h"
#include "quickyeet/Text.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {

constexpr wchar_t kWindowClass[] = L"QuickYeet.Popup.Window";
constexpr int kWindowWidth = 520;
constexpr int kWindowHeight = 560;
constexpr int kListId = 100;
constexpr int kBackId = 101;
constexpr int kCloseId = 102;
constexpr int kBrowseId = 104;
constexpr int kNewFolderId = 105;
constexpr int kOpenConfigFolderId = 106;
constexpr int kPinMenuId = 201;
constexpr int kAliasMenuId = 202;
constexpr int kRemoveMenuId = 203;
constexpr UINT kActivateEntryMessage = WM_APP + 1;

int ScaleForDpi(const int value, const UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

enum class EntryKind {
    header,
    saved_destination,
    folder,
    current_folder,
    recycle_bin,
    empty
};

enum class EntryTarget {
    none,
    yeet,
    navigate
};

struct Entry {
    EntryKind kind;
    std::wstring title;
    fs::path path;
    bool pinned = false;
};

struct Breadcrumb {
    fs::path path;
    std::wstring label;
};

bool HasYeetButton(const EntryKind kind) {
    return kind == EntryKind::saved_destination || kind == EntryKind::folder ||
           kind == EntryKind::current_folder || kind == EntryKind::recycle_bin;
}

bool IsNavigable(const EntryKind kind) {
    return kind == EntryKind::saved_destination || kind == EntryKind::folder;
}

bool IsManageableFolder(const EntryKind kind) {
    return kind == EntryKind::saved_destination || kind == EntryKind::folder ||
           kind == EntryKind::current_folder;
}

std::wstring DestinationTitle(const quickyeet::DestinationRecord& destination) {
    if (!destination.alias.empty()) {
        return destination.alias;
    }
    const std::wstring filename = destination.path.filename().wstring();
    return filename.empty() ? destination.path.wstring() : filename;
}

bool IsValidFolderName(const std::wstring& name) {
    if (name.empty() || name == L"." || name == L".." || name.back() == L'.' || name.back() == L' ') {
        return false;
    }
    constexpr wchar_t invalid[] = L"<>:\"/\\|?*";
    return std::none_of(name.begin(), name.end(), [](const wchar_t character) {
        return character < 32 || wcschr(invalid, character) != nullptr;
    });
}

std::vector<fs::path> ChildDirectories(const fs::path& parent) {
    std::vector<fs::path> directories;
    std::error_code error;
    fs::directory_iterator iterator(parent, fs::directory_options::skip_permission_denied, error);
    const fs::directory_iterator end;
    while (!error && iterator != end && directories.size() < 200) {
        std::error_code type_error;
        if (iterator->is_directory(type_error) && !type_error) {
            directories.push_back(iterator->path());
        }
        iterator.increment(error);
    }
    std::sort(directories.begin(), directories.end(), [](const fs::path& left, const fs::path& right) {
        return _wcsicmp(left.filename().c_str(), right.filename().c_str()) < 0;
    });
    return directories;
}

class PopupWindow {
public:
    explicit PopupWindow(std::vector<fs::path> sources) : sources_(std::move(sources)) {}

    ~PopupWindow() {
        if (surface_brush_ != nullptr) {
            DeleteObject(surface_brush_);
        }
        if (background_brush_ != nullptr) {
            DeleteObject(background_brush_);
        }
    }

    bool Create(HINSTANCE instance) {
        instance_ = instance;
        background_brush_ = CreateSolidBrush(RGB(246, 248, 251));
        surface_brush_ = CreateSolidBrush(RGB(255, 255, 255));
        if (background_brush_ == nullptr || surface_brush_ == nullptr) {
            return false;
        }
        WNDCLASSEXW window_class{sizeof(window_class)};
        window_class.style = CS_HREDRAW | CS_VREDRAW;
        window_class.lpfnWndProc = WindowProcedure;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = background_brush_;
        window_class.lpszClassName = kWindowClass;
        if (RegisterClassExW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        POINT cursor{};
        GetCursorPos(&cursor);
        HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitor_info{sizeof(monitor_info)};
        GetMonitorInfoW(monitor, &monitor_info);
        const int work_left = static_cast<int>(monitor_info.rcWork.left);
        const int work_top = static_cast<int>(monitor_info.rcWork.top);
        const int work_right = static_cast<int>(monitor_info.rcWork.right);
        const int work_bottom = static_cast<int>(monitor_info.rcWork.bottom);
        const int cursor_x = static_cast<int>(cursor.x);
        const int cursor_y = static_cast<int>(cursor.y);
        dpi_ = GetDpiForSystem();
        constexpr DWORD window_style = WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN;
        constexpr DWORD extended_style = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT;
        RECT window_bounds{0, 0, Scale(kWindowWidth), Scale(kWindowHeight)};
        AdjustWindowRectExForDpi(&window_bounds, window_style, FALSE, extended_style, dpi_);
        const int window_width = window_bounds.right - window_bounds.left;
        const int window_height = window_bounds.bottom - window_bounds.top;
        const int x = (std::min)(cursor_x + Scale(12), work_right - window_width);
        const int y = (std::min)(cursor_y + Scale(12), work_bottom - window_height);

        window_ = CreateWindowExW(extended_style, kWindowClass, L"QuickYeet", window_style,
                                  (std::max)(x, work_left), (std::max)(y, work_top),
                                  window_width, window_height,
                                  nullptr, nullptr, instance, this);
        if (window_ == nullptr) {
            return false;
        }

        constexpr DWM_WINDOW_CORNER_PREFERENCE rounded = DWMWCP_ROUND;
        DwmSetWindowAttribute(window_, DWMWA_WINDOW_CORNER_PREFERENCE, &rounded, sizeof(rounded));
        ShowWindow(window_, SW_SHOWNORMAL);
        UpdateWindow(window_);
        SetForegroundWindow(window_);
        return true;
    }

private:
    int Scale(const int value) const { return ScaleForDpi(value, dpi_); }

    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
        PopupWindow* self = reinterpret_cast<PopupWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
            self = static_cast<PopupWindow*>(create->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self == nullptr ? DefWindowProcW(window, message, w_param, l_param)
                               : self->HandleMessage(message, w_param, l_param);
    }

    static LRESULT CALLBACK ListProcedure(HWND window, UINT message, WPARAM w_param,
                                          LPARAM l_param, UINT_PTR, DWORD_PTR reference) {
        auto* self = reinterpret_cast<PopupWindow*>(reference);
        return self == nullptr ? DefSubclassProc(window, message, w_param, l_param)
                               : self->HandleListMessage(message, w_param, l_param);
    }

    LRESULT HandleMessage(UINT message, WPARAM w_param, LPARAM l_param) try {
        switch (message) {
            case WM_CREATE:
                dpi_ = GetDpiForWindow(window_);
                CreateControls();
                ShowRoot();
                return 0;
            case WM_SIZE:
                LayoutControls(LOWORD(l_param), HIWORD(l_param));
                return 0;
            case WM_COMMAND:
                HandleCommand(LOWORD(w_param), HIWORD(w_param));
                return 0;
            case WM_CONTEXTMENU:
                if (reinterpret_cast<HWND>(w_param) == list_) {
                    ShowEntryMenu(GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param));
                }
                return 0;
            case kActivateEntryMessage:
                ActivateEntry(static_cast<size_t>(w_param),
                              static_cast<EntryTarget>(l_param));
                return 0;
            case WM_DRAWITEM:
                DrawEntry(*reinterpret_cast<DRAWITEMSTRUCT*>(l_param));
                return TRUE;
            case WM_MEASUREITEM:
                reinterpret_cast<MEASUREITEMSTRUCT*>(l_param)->itemHeight = Scale(48);
                return TRUE;
            case WM_CTLCOLORSTATIC: {
                const HDC device = reinterpret_cast<HDC>(w_param);
                SetBkMode(device, TRANSPARENT);
                SetTextColor(device, RGB(31, 41, 55));
                return reinterpret_cast<LRESULT>(background_brush_);
            }
            case WM_CTLCOLORLISTBOX: {
                const HDC device = reinterpret_cast<HDC>(w_param);
                SetBkColor(device, RGB(255, 255, 255));
                return reinterpret_cast<LRESULT>(surface_brush_);
            }
            case WM_PAINT:
                PaintBackground();
                return 0;
            case WM_GETMINMAXINFO: {
                auto* limits = reinterpret_cast<MINMAXINFO*>(l_param);
                limits->ptMinTrackSize.x = Scale(440);
                limits->ptMinTrackSize.y = Scale(480);
                return 0;
            }
            case WM_DPICHANGED: {
                dpi_ = HIWORD(w_param);
                CreateFonts();
                const auto* suggested = reinterpret_cast<RECT*>(l_param);
                SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOACTIVATE | SWP_NOZORDER);
                RECT client{};
                GetClientRect(window_, &client);
                LayoutControls(client.right, client.bottom);
                return 0;
            }
            case WM_KEYDOWN:
                if (w_param == VK_ESCAPE) {
                    DestroyWindow(window_);
                }
                return 0;
            case WM_NCHITTEST: {
                const LRESULT hit = DefWindowProcW(window_, message, w_param, l_param);
                if (hit == HTCLIENT) {
                    POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
                    ScreenToClient(window_, &point);
                    RECT client{};
                    GetClientRect(window_, &client);
                    if (point.y < Scale(52) && point.x < client.right - Scale(64)) {
                        return HTCAPTION;
                    }
                }
                return hit;
            }
            case WM_DESTROY:
                if (list_ != nullptr) {
                    RemoveWindowSubclass(list_, ListProcedure, 1);
                }
                DeleteFonts();
                PostQuitMessage(0);
                return 0;
            default:
                return DefWindowProcW(window_, message, w_param, l_param);
        }
    } catch (const std::exception&) {
        MessageBoxW(window_, L"QuickYeet could not update the popup.", L"QuickYeet",
                    MB_OK | MB_ICONERROR);
        return 0;
    }

    void CreateControls() {
        title_ = CreateWindowExW(0, L"STATIC", L"QuickYeet",
                                 WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                                 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
        breadcrumb_ = CreateWindowExW(0, L"STATIC", L"Destinations",
                                      WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                                      0, 0, 0, 0, window_, nullptr, instance_, nullptr);
        back_ = CreateWindowExW(0, L"BUTTON", L"\x2190", WS_CHILD | BS_FLAT,
                                0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBackId)), instance_, nullptr);
        close_ = CreateWindowExW(0, L"BUTTON", L"\x00D7", WS_CHILD | WS_VISIBLE | BS_FLAT,
                                 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCloseId)), instance_, nullptr);
        list_ = CreateWindowExW(0, L"LISTBOX", nullptr,
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY |
                                    LBS_OWNERDRAWVARIABLE |
                                    LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                                0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListId)), instance_, nullptr);
        browse_ = CreateWindowExW(0, L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBrowseId)), instance_, nullptr);
        new_folder_ = CreateWindowExW(0, L"BUTTON", L"New Folder", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kNewFolderId)), instance_, nullptr);
        open_config_folder_ = CreateWindowExW(
            0, L"BUTTON", L"Open Config Folder", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOpenConfigFolderId)), instance_, nullptr);

        for (const HWND control :
             {back_, close_, list_, browse_, new_folder_, open_config_folder_}) {
            SetWindowTheme(control, L"Explorer", nullptr);
        }
        SetWindowSubclass(list_, ListProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
        CreateFonts();
    }

    void LayoutControls(int width, int height) {
        const int margin = Scale(18);
        const int close_size = Scale(34);
        MoveWindow(title_, margin, Scale(15), width - margin * 2 - close_size, Scale(34), TRUE);
        MoveWindow(close_, width - margin - close_size, Scale(12), close_size, close_size, TRUE);
        const int breadcrumb_left = breadcrumbs_.empty() ? margin : margin + Scale(42);
        MoveWindow(back_, margin, Scale(53), Scale(32), Scale(30), TRUE);
        MoveWindow(breadcrumb_, breadcrumb_left, Scale(55), width - breadcrumb_left - margin,
                   Scale(28), TRUE);

        const int list_top = Scale(92);
        const int button_height = Scale(38);
        const int config_button_top = height - margin - button_height;
        const int button_top = config_button_top - Scale(10) - button_height;
        list_frame_ = {margin, list_top, width - margin, button_top - Scale(12)};
        const int frame_inset = Scale(1);
        const int list_width = list_frame_.right - list_frame_.left - frame_inset * 2;
        const int list_height = list_frame_.bottom - list_frame_.top - frame_inset * 2;
        MoveWindow(list_, list_frame_.left + frame_inset, list_frame_.top + frame_inset,
                   list_width, list_height, TRUE);
        HRGN rounded = CreateRoundRectRgn(0, 0, list_width + 1, list_height + 1,
                                          Scale(12), Scale(12));
        if (SetWindowRgn(list_, rounded, TRUE) == 0) {
            DeleteObject(rounded);
        }

        const int button_width = (width - margin * 2 - Scale(10)) / 2;
        MoveWindow(browse_, margin, button_top, button_width, button_height, TRUE);
        MoveWindow(new_folder_, margin + button_width + Scale(10), button_top,
                   button_width, button_height, TRUE);
        MoveWindow(open_config_folder_, margin, config_button_top, width - margin * 2,
                   button_height, TRUE);
        InvalidateRect(window_, nullptr, FALSE);
    }

    void CreateFonts() {
        DeleteFonts();
        auto create_font = [this](const int points, const int weight) {
            return CreateFontW(-MulDiv(points, static_cast<int>(dpi_), 72), 0, 0, 0, weight,
                               FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
        };
        title_font_ = create_font(15, FW_SEMIBOLD);
        body_font_ = create_font(10, FW_NORMAL);
        semibold_font_ = create_font(10, FW_SEMIBOLD);
        small_font_ = create_font(9, FW_NORMAL);

        for (const HWND control :
             {back_, list_, browse_, new_folder_, open_config_folder_}) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(body_font_), TRUE);
        }
        SendMessageW(title_, WM_SETFONT, reinterpret_cast<WPARAM>(title_font_), TRUE);
        SendMessageW(breadcrumb_, WM_SETFONT, reinterpret_cast<WPARAM>(semibold_font_), TRUE);
        SendMessageW(close_, WM_SETFONT, reinterpret_cast<WPARAM>(title_font_), TRUE);
    }

    void PaintBackground() const {
        PAINTSTRUCT paint{};
        const HDC device = BeginPaint(window_, &paint);
        FillRect(device, &paint.rcPaint, background_brush_);
        if (list_frame_.right > list_frame_.left && list_frame_.bottom > list_frame_.top) {
            HPEN border = CreatePen(PS_SOLID, Scale(1), RGB(218, 224, 232));
            const HGDIOBJ old_pen = SelectObject(device, border);
            const HGDIOBJ old_brush = SelectObject(device, surface_brush_);
            RoundRect(device, list_frame_.left, list_frame_.top, list_frame_.right,
                      list_frame_.bottom, Scale(14), Scale(14));
            SelectObject(device, old_brush);
            SelectObject(device, old_pen);
            DeleteObject(border);
        }
        EndPaint(window_, &paint);
    }

    void DeleteFonts() {
        for (HFONT font : {title_font_, body_font_, semibold_font_, small_font_}) {
            if (font != nullptr) {
                DeleteObject(font);
            }
        }
        title_font_ = nullptr;
        body_font_ = nullptr;
        semibold_font_ = nullptr;
        small_font_ = nullptr;
    }

    void ShowRoot() {
        breadcrumbs_.clear();
        entries_.clear();
        std::wstring load_error;
        const bool loaded = history_.Load(&load_error);

        entries_.push_back({EntryKind::header, L"RECENT"});
        const auto recent = loaded ? history_.Recent(8) : std::vector<quickyeet::DestinationRecord>{};
        if (!loaded) {
            entries_.push_back({EntryKind::empty, L"Configuration unavailable"});
        } else if (recent.empty()) {
            entries_.push_back({EntryKind::empty, L"No recent destinations yet"});
        } else {
            for (const auto& destination : recent) {
                entries_.push_back({EntryKind::saved_destination, DestinationTitle(destination),
                                    destination.path, destination.pinned});
            }
        }

        entries_.push_back({EntryKind::header, L"PINNED"});
        const auto pinned = loaded ? history_.Pinned() : std::vector<quickyeet::DestinationRecord>{};
        if (!loaded) {
            entries_.push_back({EntryKind::empty, L"Configuration unavailable"});
        } else if (pinned.empty()) {
            entries_.push_back({EntryKind::empty, L"No pinned destinations"});
        } else {
            for (const auto& destination : pinned) {
                entries_.push_back({EntryKind::saved_destination, DestinationTitle(destination),
                                    destination.path, destination.pinned});
            }
        }
        if (history_.RecycleBinEnabled()) {
            entries_.push_back({EntryKind::recycle_bin, L"Recycle Bin"});
        }
        RefreshList(L"Destinations");
    }

    void ShowCurrentFolder() {
        entries_.clear();
        const Breadcrumb& current = breadcrumbs_.back();
        const auto current_record = history_.Find(current.path);
        entries_.push_back({EntryKind::current_folder, L"This folder", current.path,
                            current_record && current_record->pinned});
        for (const auto& child : ChildDirectories(current.path)) {
            const auto record = history_.Find(child);
            entries_.push_back({EntryKind::folder,
                                record ? DestinationTitle(*record) : child.filename().wstring(),
                                child, record && record->pinned});
        }

        std::wstring trail = L"QuickYeet";
        for (const auto& crumb : breadcrumbs_) {
            trail += L" / " + crumb.label;
        }
        RefreshList(trail);
    }

    void RefreshList(const std::wstring& breadcrumb) {
        SetWindowTextW(breadcrumb_, breadcrumb.c_str());
        ShowWindow(back_, breadcrumbs_.empty() ? SW_HIDE : SW_SHOW);
        pressed_index_.reset();
        pressed_target_ = EntryTarget::none;
        press_cancelled_ = false;
        hot_index_.reset();
        hot_target_ = EntryTarget::none;
        SendMessageW(list_, LB_RESETCONTENT, 0, 0);
        for (const auto& entry : entries_) {
            const LRESULT item =
                SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry.title.c_str()));
            if (item != LB_ERR && item != LB_ERRSPACE) {
                int item_height = 58;
                if (entry.kind == EntryKind::header) {
                    item_height = 38;
                } else if (entry.kind == EntryKind::empty) {
                    item_height = 44;
                }
                SendMessageW(list_, LB_SETITEMHEIGHT, static_cast<WPARAM>(item),
                             Scale(item_height));
            }
        }
        RECT client{};
        GetClientRect(window_, &client);
        LayoutControls(client.right, client.bottom);
        InvalidateRect(list_, nullptr, TRUE);
    }

    RECT YeetButtonRect(const RECT& item_rect) const {
        constexpr int button_width = 64;
        constexpr int button_height = 32;
        RECT button{};
        button.left = item_rect.left + Scale(12);
        button.right = button.left + Scale(button_width);
        button.top = item_rect.top +
                     ((item_rect.bottom - item_rect.top) - Scale(button_height)) / 2;
        button.bottom = button.top + Scale(button_height);
        return button;
    }

    void DrawEntry(const DRAWITEMSTRUCT& draw) const {
        if (draw.CtlID != kListId || draw.itemID == static_cast<UINT>(-1) || draw.itemID >= entries_.size()) {
            return;
        }
        const Entry& entry = entries_[draw.itemID];
        const bool actionable = HasYeetButton(entry.kind) || IsNavigable(entry.kind);
        const bool selected = actionable && (draw.itemState & ODS_SELECTED) != 0;
        const bool body_hot = hot_index_ && *hot_index_ == draw.itemID &&
                              hot_target_ == EntryTarget::navigate;
        const bool body_pressed = pressed_index_ && *pressed_index_ == draw.itemID &&
                                  pressed_target_ == EntryTarget::navigate && !press_cancelled_;
        const COLORREF background = body_pressed ? RGB(214, 232, 247)
                                  : selected || body_hot ? RGB(229, 241, 251)
                                                         : RGB(255, 255, 255);
        HBRUSH brush = CreateSolidBrush(background);
        FillRect(draw.hDC, &draw.rcItem, brush);
        DeleteObject(brush);
        SetBkMode(draw.hDC, TRANSPARENT);

        if (HasYeetButton(entry.kind)) {
            const bool button_hot = hot_index_ && *hot_index_ == draw.itemID &&
                                    hot_target_ == EntryTarget::yeet;
            const bool button_pressed = pressed_index_ && *pressed_index_ == draw.itemID &&
                                        pressed_target_ == EntryTarget::yeet && !press_cancelled_;
            const COLORREF button_color = button_pressed ? RGB(0, 72, 140)
                                        : button_hot ? RGB(0, 109, 204)
                                                     : RGB(0, 95, 184);
            HBRUSH button_brush = CreateSolidBrush(button_color);
            HPEN button_pen = CreatePen(PS_SOLID, Scale(1), button_color);
            const HGDIOBJ old_brush = SelectObject(draw.hDC, button_brush);
            const HGDIOBJ old_pen = SelectObject(draw.hDC, button_pen);
            const RECT button_rect = YeetButtonRect(draw.rcItem);
            RoundRect(draw.hDC, button_rect.left, button_rect.top, button_rect.right,
                      button_rect.bottom, Scale(8), Scale(8));
            SelectObject(draw.hDC, old_pen);
            SelectObject(draw.hDC, old_brush);
            DeleteObject(button_pen);
            DeleteObject(button_brush);

            RECT button_text = button_rect;
            SelectObject(draw.hDC, semibold_font_);
            SetTextColor(draw.hDC, RGB(255, 255, 255));
            DrawTextW(draw.hDC, L"Yeet", -1, &button_text,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }

        RECT title_rect = draw.rcItem;
        title_rect.left += Scale(HasYeetButton(entry.kind) ? 92 : 16);
        title_rect.right -= Scale(32);
        title_rect.top += Scale(entry.kind == EntryKind::header ? 12 :
                                entry.kind == EntryKind::empty ? 11 : 8);
        SelectObject(draw.hDC, entry.kind == EntryKind::header ? semibold_font_ : body_font_);
        SetTextColor(draw.hDC, entry.kind == EntryKind::header
                                       ? RGB(0, 95, 184)
                                       : entry.kind == EntryKind::empty ? RGB(100, 116, 139)
                                                                         : RGB(31, 41, 55));
        DrawTextW(draw.hDC, entry.title.c_str(), -1, &title_rect, DT_SINGLELINE | DT_END_ELLIPSIS);

        if (!entry.path.empty()) {
            RECT path_rect = draw.rcItem;
            path_rect.left += Scale(HasYeetButton(entry.kind) ? 92 : 16);
            path_rect.right -= Scale(32);
            path_rect.top += Scale(31);
            SelectObject(draw.hDC, small_font_);
            SetTextColor(draw.hDC, RGB(100, 116, 139));
            const std::wstring path = entry.path.wstring();
            DrawTextW(draw.hDC, path.c_str(), -1, &path_rect, DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        if (entry.kind == EntryKind::saved_destination || entry.kind == EntryKind::folder) {
            RECT arrow = draw.rcItem;
            arrow.left = arrow.right - Scale(28);
            arrow.top += Scale(17);
            SetTextColor(draw.hDC, RGB(100, 116, 139));
            DrawTextW(draw.hDC, L">", 1, &arrow, DT_SINGLELINE);
        }
        if ((draw.itemState & ODS_FOCUS) != 0) {
            DrawFocusRect(draw.hDC, &draw.rcItem);
        }
    }

    std::optional<size_t> ItemAtPoint(const POINT point) const {
        const LRESULT hit = SendMessageW(
            list_, LB_ITEMFROMPOINT, 0,
            MAKELPARAM(static_cast<short>(point.x), static_cast<short>(point.y)));
        if (HIWORD(hit) != 0) {
            return std::nullopt;
        }
        const size_t index = LOWORD(hit);
        if (index >= entries_.size()) {
            return std::nullopt;
        }
        RECT item_rect{};
        if (SendMessageW(list_, LB_GETITEMRECT, index,
                         reinterpret_cast<LPARAM>(&item_rect)) == LB_ERR ||
            !PtInRect(&item_rect, point)) {
            return std::nullopt;
        }
        return index;
    }

    EntryTarget TargetAtPoint(const POINT point, std::optional<size_t>* item = nullptr) const {
        const auto index = ItemAtPoint(point);
        if (item != nullptr) {
            *item = index;
        }
        if (!index) {
            return EntryTarget::none;
        }

        RECT item_rect{};
        SendMessageW(list_, LB_GETITEMRECT, *index, reinterpret_cast<LPARAM>(&item_rect));
        const Entry& entry = entries_[*index];
        if (HasYeetButton(entry.kind)) {
            const RECT button_rect = YeetButtonRect(item_rect);
            if (PtInRect(&button_rect, point)) {
                return EntryTarget::yeet;
            }
        }
        return IsNavigable(entry.kind) ? EntryTarget::navigate : EntryTarget::none;
    }

    void SetHotTarget(const std::optional<size_t> index, const EntryTarget target) {
        const std::optional<size_t> actionable_index =
            target == EntryTarget::none ? std::nullopt : index;
        if (hot_index_ == actionable_index && hot_target_ == target) {
            return;
        }
        hot_index_ = actionable_index;
        hot_target_ = target;
        InvalidateRect(list_, nullptr, FALSE);
    }

    LRESULT HandleListMessage(const UINT message, const WPARAM w_param,
                              const LPARAM l_param) {
        switch (message) {
            case WM_LBUTTONDOWN: {
                suppress_next_release_ = false;
                SetFocus(list_);
                const POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
                std::optional<size_t> index;
                const EntryTarget target = TargetAtPoint(point, &index);
                if (index && (HasYeetButton(entries_[*index].kind) ||
                              IsNavigable(entries_[*index].kind))) {
                    SendMessageW(list_, LB_SETCURSEL, *index, 0);
                    InvalidateRect(list_, nullptr, FALSE);
                }
                if (target != EntryTarget::none && index) {
                    pressed_index_ = index;
                    pressed_target_ = target;
                    press_cancelled_ = false;
                    SetCapture(list_);
                    InvalidateRect(list_, nullptr, FALSE);
                }
                return 0;
            }
            case WM_LBUTTONDBLCLK:
                suppress_next_release_ = true;
                return 0;
            case WM_LBUTTONUP: {
                if (suppress_next_release_) {
                    suppress_next_release_ = false;
                    return 0;
                }
                const auto pressed_index = pressed_index_;
                const EntryTarget pressed_target = pressed_target_;
                const bool cancelled = press_cancelled_;
                pressed_index_.reset();
                pressed_target_ = EntryTarget::none;
                press_cancelled_ = false;
                if (GetCapture() == list_) {
                    ReleaseCapture();
                }
                InvalidateRect(list_, nullptr, FALSE);

                const POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
                std::optional<size_t> released_index;
                const EntryTarget released_target = TargetAtPoint(point, &released_index);
                if (!cancelled && pressed_index && released_index == pressed_index &&
                    released_target == pressed_target) {
                    SendMessageW(window_, kActivateEntryMessage, *pressed_index,
                                 static_cast<LPARAM>(pressed_target));
                }
                return 0;
            }
            case WM_MOUSEMOVE: {
                const POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
                std::optional<size_t> index;
                const EntryTarget target = TargetAtPoint(point, &index);
                SetHotTarget(index, target);
                if (pressed_index_ &&
                    (index != pressed_index_ || target != pressed_target_)) {
                    press_cancelled_ = true;
                    InvalidateRect(list_, nullptr, FALSE);
                }
                if (!tracking_mouse_leave_) {
                    TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, list_, 0};
                    tracking_mouse_leave_ = TrackMouseEvent(&tracking) != FALSE;
                }
                return 0;
            }
            case WM_MOUSELEAVE:
                tracking_mouse_leave_ = false;
                SetHotTarget(std::nullopt, EntryTarget::none);
                if (pressed_index_) {
                    press_cancelled_ = true;
                    InvalidateRect(list_, nullptr, FALSE);
                }
                return 0;
            case WM_CAPTURECHANGED:
                pressed_index_.reset();
                pressed_target_ = EntryTarget::none;
                press_cancelled_ = false;
                InvalidateRect(list_, nullptr, FALSE);
                return 0;
            case WM_KEYDOWN: {
                if (w_param == VK_ESCAPE) {
                    DestroyWindow(window_);
                    return 0;
                }
                if ((w_param == VK_RETURN || w_param == VK_SPACE) &&
                    (l_param & (1LL << 30)) == 0) {
                    const auto index = SelectedIndex();
                    if (index) {
                        EntryTarget target = EntryTarget::none;
                        if (w_param == VK_SPACE && HasYeetButton(entries_[*index].kind)) {
                            target = EntryTarget::yeet;
                        } else if (w_param == VK_RETURN) {
                            target = IsNavigable(entries_[*index].kind)
                                         ? EntryTarget::navigate
                                         : HasYeetButton(entries_[*index].kind)
                                               ? EntryTarget::yeet
                                               : EntryTarget::none;
                        }
                        if (target != EntryTarget::none) {
                            SendMessageW(window_, kActivateEntryMessage, *index,
                                         static_cast<LPARAM>(target));
                        }
                    }
                    return 0;
                }
                break;
            }
            case WM_CHAR:
                if (w_param == VK_RETURN || w_param == VK_SPACE) {
                    return 0;
                }
                break;
            case WM_SETCURSOR: {
                POINT point{};
                GetCursorPos(&point);
                ScreenToClient(list_, &point);
                if (TargetAtPoint(point) != EntryTarget::none) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
                break;
            }
            case WM_SETFOCUS:
            case WM_KILLFOCUS:
                InvalidateRect(list_, nullptr, FALSE);
                break;
            default:
                break;
        }
        return DefSubclassProc(list_, message, w_param, l_param);
    }

    void HandleCommand(int identifier, int) {
        if (identifier == kCloseId) {
            DestroyWindow(window_);
        } else if (identifier == kBackId) {
            breadcrumbs_.pop_back();
            breadcrumbs_.empty() ? ShowRoot() : ShowCurrentFolder();
        } else if (identifier == kBrowseId) {
            Browse();
        } else if (identifier == kNewFolderId) {
            CreateNewFolder();
        } else if (identifier == kOpenConfigFolderId) {
            OpenConfigFolder();
        } else if (identifier == kPinMenuId) {
            ManageSelected(kPinMenuId);
        } else if (identifier == kAliasMenuId) {
            ManageSelected(kAliasMenuId);
        } else if (identifier == kRemoveMenuId) {
            ManageSelected(kRemoveMenuId);
        }
    }

    std::optional<size_t> SelectedIndex() const {
        const LRESULT selection = SendMessageW(list_, LB_GETCURSEL, 0, 0);
        if (selection == LB_ERR || static_cast<size_t>(selection) >= entries_.size()) {
            return std::nullopt;
        }
        return static_cast<size_t>(selection);
    }

    void ActivateEntry(const size_t index, const EntryTarget target) {
        if (index >= entries_.size()) {
            return;
        }
        const Entry entry = entries_[index];
        if (target == EntryTarget::navigate && IsNavigable(entry.kind)) {
            breadcrumbs_.push_back({entry.path, entry.title});
            ShowCurrentFolder();
        } else if (target == EntryTarget::yeet && entry.kind != EntryKind::recycle_bin &&
                   HasYeetButton(entry.kind)) {
            MoveFilesTo(entry.path);
        } else if (target == EntryTarget::yeet && entry.kind == EntryKind::recycle_bin) {
            RecycleSelection();
        }
    }

    void Browse() {
        const std::wstring initial = breadcrumbs_.empty() ? L"" : breadcrumbs_.back().path.wstring();
        const auto destination = BrowseForDestination(window_, initial);
        if (destination) {
            MoveFilesTo(*destination);
        }
    }

    void CreateNewFolder() {
        fs::path parent;
        if (breadcrumbs_.empty()) {
            const auto selected = BrowseForDestination(window_);
            if (!selected) {
                return;
            }
            parent = *selected;
        } else {
            parent = breadcrumbs_.back().path;
        }

        const auto name = ShowTextPrompt(window_, L"New Folder", L"Folder name:");
        if (!name) {
            return;
        }
        if (!IsValidFolderName(*name)) {
            MessageBoxW(window_, L"Enter a Windows folder name without reserved characters or a trailing dot or space.",
                        L"QuickYeet", MB_OK | MB_ICONWARNING);
            return;
        }
        const fs::path folder = parent / *name;
        if (!CreateDirectoryW(folder.c_str(), nullptr)) {
            const std::wstring error = L"QuickYeet could not create the folder:\n\n" +
                                       quickyeet::WindowsErrorMessage(GetLastError());
            MessageBoxW(window_, error.c_str(), L"QuickYeet", MB_OK | MB_ICONERROR);
            return;
        }
        MoveFilesTo(folder);
    }

    void OpenConfigFolder() {
        std::wstring error;
        if (!quickyeet::OpenConfigFolder(&error)) {
            MessageBoxW(window_, error.c_str(), L"QuickYeet", MB_OK | MB_ICONERROR);
        }
    }

    void RecycleSelection() {
        const unsigned long error = quickyeet::RecycleFiles(sources_);
        if (error != ERROR_SUCCESS) {
            const std::wstring message =
                L"QuickYeet could not send the selected files to Recycle Bin:\n\n" +
                quickyeet::WindowsErrorMessage(error);
            MessageBoxW(window_, message.c_str(), L"QuickYeet", MB_OK | MB_ICONERROR);
            return;
        }
        DestroyWindow(window_);
    }

    void MoveFilesTo(const fs::path& destination) {
        const auto results = quickyeet::MoveFilesWithCollisionSuffix(sources_, destination);
        std::wstring failures;
        size_t succeeded = 0;
        for (const auto& result : results) {
            if (result.succeeded()) {
                ++succeeded;
            } else {
                failures += L"\n" + result.source.filename().wstring() + L": " +
                            quickyeet::WindowsErrorMessage(result.error_code);
            }
        }
        if (succeeded > 0) {
            std::wstring history_error;
            if (history_.Load(&history_error)) {
                history_.RecordUse(destination);
                if (!history_.Save(&history_error)) {
                    MessageBoxW(window_, history_error.c_str(), L"QuickYeet",
                                MB_OK | MB_ICONERROR);
                }
            } else {
                MessageBoxW(window_, history_error.c_str(), L"QuickYeet", MB_OK | MB_ICONERROR);
            }
        }
        if (!failures.empty()) {
            const std::wstring message = L"QuickYeet could not move some selected files:" + failures;
            MessageBoxW(window_, message.c_str(), L"QuickYeet", MB_OK | MB_ICONERROR);
        }
        if (succeeded > 0) {
            DestroyWindow(window_);
        }
    }

    void ShowEntryMenu(int x, int y) {
        std::optional<size_t> index;
        if (x == -1 && y == -1) {
            RECT item_rect{};
            index = SelectedIndex();
            if (!index) {
                return;
            }
            SendMessageW(list_, LB_GETITEMRECT, *index, reinterpret_cast<LPARAM>(&item_rect));
            POINT point{item_rect.left + 24, item_rect.bottom};
            ClientToScreen(list_, &point);
            x = point.x;
            y = point.y;
        } else {
            POINT point{x, y};
            ScreenToClient(list_, &point);
            index = ItemAtPoint(point);
            if (!index) {
                return;
            }
            SendMessageW(list_, LB_SETCURSEL, *index, 0);
            InvalidateRect(list_, nullptr, FALSE);
        }
        if (!index || !IsManageableFolder(entries_[*index].kind)) {
            return;
        }
        const Entry& entry = entries_[*index];
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kPinMenuId,
                    entry.pinned ? L"Unpin folder" : L"Pin folder");
        AppendMenuW(menu, MF_STRING, kAliasMenuId, L"Set alias...");
        if (entry.kind == EntryKind::saved_destination) {
            AppendMenuW(menu, MF_STRING, kRemoveMenuId, L"Remove from list");
        }
        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, window_, nullptr);
        DestroyMenu(menu);
        if (command != 0) {
            SendMessageW(window_, WM_COMMAND, command, 0);
        }
    }

    void ManageSelected(int command) {
        const auto index = SelectedIndex();
        if (!index || !IsManageableFolder(entries_[*index].kind) ||
            (command == kRemoveMenuId &&
             entries_[*index].kind != EntryKind::saved_destination)) {
            return;
        }
        const Entry entry = entries_[*index];
        std::wstring error;
        if (!history_.Load(&error)) {
            MessageBoxW(window_, error.c_str(), L"QuickYeet", MB_OK | MB_ICONERROR);
            return;
        }
        if (command == kPinMenuId) {
            const auto record = history_.Find(entry.path);
            history_.SetPinned(entry.path, !(record && record->pinned));
        } else if (command == kAliasMenuId) {
            const auto record = history_.Find(entry.path);
            const auto alias = ShowTextPrompt(window_, L"Destination Alias",
                                              L"Alias (leave blank to use the folder name):",
                                              record ? record->alias : L"");
            if (!alias) {
                return;
            }
            history_.SetAlias(entry.path, *alias);
        } else if (command == kRemoveMenuId) {
            history_.Remove(entry.path);
        }
        if (!history_.Save(&error)) {
            MessageBoxW(window_, error.c_str(), L"QuickYeet", MB_OK | MB_ICONERROR);
        }
        breadcrumbs_.empty() ? ShowRoot() : ShowCurrentFolder();
    }

    HINSTANCE instance_ = nullptr;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    HWND window_ = nullptr;
    HWND title_ = nullptr;
    HWND breadcrumb_ = nullptr;
    HWND back_ = nullptr;
    HWND close_ = nullptr;
    HWND list_ = nullptr;
    HWND browse_ = nullptr;
    HWND new_folder_ = nullptr;
    HWND open_config_folder_ = nullptr;
    HFONT title_font_ = nullptr;
    HFONT body_font_ = nullptr;
    HFONT semibold_font_ = nullptr;
    HFONT small_font_ = nullptr;
    HBRUSH background_brush_ = nullptr;
    HBRUSH surface_brush_ = nullptr;
    RECT list_frame_{};
    std::optional<size_t> hot_index_;
    EntryTarget hot_target_ = EntryTarget::none;
    std::optional<size_t> pressed_index_;
    EntryTarget pressed_target_ = EntryTarget::none;
    bool press_cancelled_ = false;
    bool suppress_next_release_ = false;
    bool tracking_mouse_leave_ = false;
    std::vector<fs::path> sources_;
    std::vector<Entry> entries_;
    std::vector<Breadcrumb> breadcrumbs_;
    quickyeet::HistoryStore history_;
};

}  // namespace

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com_result =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(com_result)) {
        MessageBoxW(nullptr, L"QuickYeet could not initialize Windows components.", L"QuickYeet",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    int argument_count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    std::vector<fs::path> sources;
    if (arguments != nullptr) {
        for (int index = 1; index < argument_count; ++index) {
            const DWORD attributes = GetFileAttributesW(arguments[index]);
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                sources.emplace_back(arguments[index]);
            }
        }
        LocalFree(arguments);
    }
    if (sources.empty()) {
        MessageBoxW(nullptr, L"QuickYeet needs at least one file to move.", L"QuickYeet",
                    MB_OK | MB_ICONWARNING);
        CoUninitialize();
        return 1;
    }

    PopupWindow popup(std::move(sources));
    if (!popup.Create(instance)) {
        CoUninitialize();
        return 1;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
