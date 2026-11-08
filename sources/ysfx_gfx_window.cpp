// Copyright 2024 Joep Vanlier
// Copyright 2026 TuneLab contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

// A self-contained top-level window for JSFX @gfx.
//
// ysfx already owns the EEL2 VM, the framebuffer and the @gfx section.  This
// file only adds the missing piece: a native window, an event loop, and the
// input/menu/cursor/drop-file callbacks that ysfx_gfx_config_t expects.
//
// The implementation deliberately uses the platform APIs directly.  On
// Windows that is Win32; on Linux/macOS it is SWELL, which is already vendored
// and built as part of the LICE target.  No additional GUI dependency is
// introduced, so the same source builds for every RID that already builds
// ysfx.

#include "ysfx.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

#if !defined(YSFX_NO_GFX)
#   ifdef _WIN32
#       ifndef WIN32_LEAN_AND_MEAN
#           define WIN32_LEAN_AND_MEAN
#       endif
#       include <windows.h>
#       include <shellapi.h>
#   else
#       include "WDL/wdltypes.h"
#       include "WDL/swell/swell.h"
#   endif
#   include "WDL/lice/lice.h"
#endif

#if !defined(YSFX_NO_GFX)
#   ifndef _WIN32
// SWELL's public header does not declare this internal pump; it is exported
// from the SWELL objects that LICE links into the shared library.
extern void SWELL_RunEvents();
#   endif
#endif

#if !defined(YSFX_NO_GFX)

#ifdef _WIN32
static constexpr wchar_t kWindowClass[] = L"TuneLabYsfxGfxWindow";
#else
static constexpr char kWindowClass[] = "TuneLabYsfxGfxWindow";
#endif

static inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Set YSFX_GFX_WINDOW_DEBUG=1 to trace the creation path on stderr.  This is
// a debugging aid for hosts and the smoke test, not part of the API.
static bool gfx_window_debug()
{
    static const bool enabled = [] {
        const char *env = getenv("YSFX_GFX_WINDOW_DEBUG");
        return env && *env && *env != '0';
    }();
    return enabled;
}

#define YGFX_DBG(...)                          \
    do {                                       \
        if (gfx_window_debug()) {              \
            fprintf(stderr, "ysfx_gfx_window: " __VA_ARGS__); \
            fflush(stderr);                    \
        }                                      \
    } while (0)

#ifdef _WIN32
static std::wstring utf8_to_wide(const char *text)
{
    if (!text || !*text)
        return std::wstring();

    int needed = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (needed <= 1)
        return std::wstring();

    std::wstring result(static_cast<size_t>(needed - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), needed);
    return result;
}
#endif

static bool translate_special_key(uint32_t vk, uint32_t &key)
{
    switch (vk) {
    case VK_BACK: key = ysfx_key_backspace; return true;
    case VK_ESCAPE: key = ysfx_key_escape; return true;
    case VK_DELETE: key = ysfx_key_delete; return true;
    case VK_F1: key = ysfx_key_f1; return true;
    case VK_F2: key = ysfx_key_f2; return true;
    case VK_F3: key = ysfx_key_f3; return true;
    case VK_F4: key = ysfx_key_f4; return true;
    case VK_F5: key = ysfx_key_f5; return true;
    case VK_F6: key = ysfx_key_f6; return true;
    case VK_F7: key = ysfx_key_f7; return true;
    case VK_F8: key = ysfx_key_f8; return true;
    case VK_F9: key = ysfx_key_f9; return true;
    case VK_F10: key = ysfx_key_f10; return true;
    case VK_F11: key = ysfx_key_f11; return true;
    case VK_F12: key = ysfx_key_f12; return true;
    case VK_LEFT: key = ysfx_key_left; return true;
    case VK_UP: key = ysfx_key_up; return true;
    case VK_RIGHT: key = ysfx_key_right; return true;
    case VK_DOWN: key = ysfx_key_down; return true;
    case VK_PRIOR: key = ysfx_key_page_up; return true;
    case VK_NEXT: key = ysfx_key_page_down; return true;
    case VK_HOME: key = ysfx_key_home; return true;
    case VK_END: key = ysfx_key_end; return true;
    case VK_INSERT: key = ysfx_key_insert; return true;
    default: return false;
    }
}

static uint32_t current_mods()
{
    uint32_t mods = 0;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
        mods |= ysfx_mod_shift;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000)
        mods |= ysfx_mod_ctrl;
    if (GetAsyncKeyState(VK_MENU) & 0x8000)
        mods |= ysfx_mod_alt;
#ifdef VK_LWIN
    if (GetAsyncKeyState(VK_LWIN) & 0x8000)
        mods |= ysfx_mod_super;
#endif
#ifdef VK_RWIN
    if (GetAsyncKeyState(VK_RWIN) & 0x8000)
        mods |= ysfx_mod_super;
#endif
    return mods;
}

static bool build_menu_items(HMENU menu, const ysfx_menu_t *desc, uint32_t &index, int &position)
{
    while (index < desc->insn_count) {
        const ysfx_menu_insn_t &insn = desc->insns[index++];

        switch (insn.opcode) {
        case ysfx_menu_item: {
            if (insn.id == 0)
                continue;

            unsigned flags = MF_BYPOSITION | MF_STRING;
            if (insn.item_flags & ysfx_menu_item_disabled)
                flags |= MF_GRAYED;
            if (insn.item_flags & ysfx_menu_item_checked)
                flags |= MF_CHECKED;

#ifdef _WIN32
            std::wstring name = utf8_to_wide(insn.name);
            InsertMenuW(menu, position, flags, insn.id, name.c_str());
#else
            InsertMenu(menu, position, flags, insn.id, insn.name ? insn.name : "");
#endif
            ++position;
            break;
        }
        case ysfx_menu_separator:
            InsertMenu(menu, position, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
            ++position;
            break;
        case ysfx_menu_sub: {
            HMENU submenu = CreatePopupMenu();
            if (!submenu)
                return false;

            int subposition = 0;
            if (!build_menu_items(submenu, desc, index, subposition)) {
                DestroyMenu(submenu);
                return false;
            }

            unsigned flags = MF_BYPOSITION | MF_POPUP | MF_STRING;
            if (insn.item_flags & ysfx_menu_item_disabled)
                flags |= MF_GRAYED;
            if (insn.item_flags & ysfx_menu_item_checked)
                flags |= MF_CHECKED;

#ifdef _WIN32
            std::wstring name = utf8_to_wide(insn.name);
            InsertMenuW(menu, position, flags, reinterpret_cast<UINT_PTR>(submenu), name.c_str());
#else
            InsertMenu(menu, position, flags, reinterpret_cast<UINT_PTR>(submenu), insn.name ? insn.name : "");
#endif
            ++position;
            break;
        }
        case ysfx_menu_endsub:
            return true;
        }
    }

    return true;
}

#ifdef __APPLE__
#include <objc/message.h>
#include <objc/runtime.h>

// SWELL expects the host application to have set up NSApplication (REAPER
// does).  Make sure one exists so that a plain host process can open @gfx
// windows without going through swellappmain.
static void ensure_ns_application()
{
    using msg_send_t = id (*)(id, SEL);
    id cls = (id)objc_getClass("NSApplication");
    if (!cls)
        return;
    id app = ((msg_send_t)&objc_msgSend)(cls, sel_registerName("sharedApplication"));
    (void)app;
}
#endif

struct ysfx_gfx_window_s {
    ysfx_t *fx = nullptr;
    ysfx_gfx_window_desc_t desc{};
    std::string title;

#if defined(_WIN32)
    HWND hwnd = nullptr;
#else
    HWND hwnd = nullptr;
#endif

    std::unique_ptr<LICE_SysBitmap> framebuffer;
    uint32_t fb_width = 0;
    uint32_t fb_height = 0;
    uint32_t client_width = 0;
    uint32_t client_height = 0;
    double display_scale = 1.0;
    double pixel_scale = 1.0;
    bool wants_retina = false;
    bool need_setup = true;
    bool force_paint = true;
    bool open = false;

    int mouse_client_x = 0;
    int mouse_client_y = 0;
    bool mouse_over = false;
    uint32_t mouse_buttons = 0;
    double wheel = 0.0;
    double hwheel = 0.0;

    uint32_t last_vk = 0;
    uint32_t suppress_char_vk = 0;
    std::vector<std::pair<uint32_t, uint32_t>> key_map; // VK -> ysfx key currently down
    std::vector<std::string> drop_files;

    HCURSOR cursor = nullptr;
    int32_t cursor_resid = 0;

    std::atomic<bool> close_requested{false};
    bool close_posted = false;

    std::chrono::steady_clock::time_point next_render{};

    explicit ysfx_gfx_window_s(ysfx_t *effect, const ysfx_gfx_window_desc_t *d)
        : fx(effect)
        , desc(d ? *d : ysfx_gfx_window_desc_t{})
    {
        if (fx)
            ysfx_add_ref(fx);

        if (desc.title && *desc.title)
            title = desc.title;
        else if (fx && ysfx_get_name(fx))
            title = ysfx_get_name(fx);
        else
            title = "JSFX";

        display_scale = desc.scale_factor > 0.0 ? desc.scale_factor : 1.0;
        wants_retina = fx && ysfx_gfx_wants_retina(fx);
        pixel_scale = wants_retina ? display_scale : 1.0;

        if (pixel_scale <= 0.0)
            pixel_scale = 1.0;

        next_render = std::chrono::steady_clock::now();
    }

    ~ysfx_gfx_window_s()
    {
        framebuffer.reset();

        if (fx) {
            ysfx_free(fx);
            fx = nullptr;
        }
    }

    // ── native window ─────────────────────────────────────────────────────

#ifdef _WIN32
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        ysfx_gfx_window_s *self = reinterpret_cast<ysfx_gfx_window_s *>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        if (msg == WM_NCCREATE) {
            auto *cs = reinterpret_cast<CREATESTRUCTW *>(lparam);
            self = static_cast<ysfx_gfx_window_s *>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }

        return self ? self->handle_message(hwnd, msg, wparam, lparam)
                    : DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    bool create_native()
    {
        static bool registered = false;

        if (!registered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.style = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc = &ysfx_gfx_window_s::window_proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
            wc.hbrBackground = nullptr;
            wc.lpszClassName = kWindowClass;

            if (!RegisterClassExW(&wc))
                return false;

            registered = true;
        }

        DWORD style = WS_OVERLAPPEDWINDOW;
        if (!(desc.flags & ysfx_gfx_window_flag_resizable))
            style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);

        int width = clampi(desc.width ? static_cast<int>(desc.width) : 640, 16, 8192);
        int height = clampi(desc.height ? static_cast<int>(desc.height) : 480, 16, 8192);

        RECT rect{0, 0, width, height};
        AdjustWindowRectEx(&rect, style, FALSE, WS_EX_ACCEPTFILES);

        std::wstring wtitle = utf8_to_wide(title.c_str());

        hwnd = CreateWindowExW(
            WS_EX_ACCEPTFILES,
            kWindowClass,
            wtitle.c_str(),
            style,
            CW_USEDEFAULT, CW_USEDEFAULT,
            rect.right - rect.left,
            rect.bottom - rect.top,
            nullptr, nullptr, GetModuleHandleW(nullptr), this);

        if (!hwnd)
            return false;

        DragAcceptFiles(hwnd, TRUE);
        cursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        return true;
    }

    void destroy_native()
    {
        if (hwnd) {
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }
    }

    void set_native_title()
    {
        if (!hwnd)
            return;

        std::wstring wtitle = utf8_to_wide(title.c_str());
        SetWindowTextW(hwnd, wtitle.c_str());
    }
#else
    static INT_PTR window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        ysfx_gfx_window_s *self = reinterpret_cast<ysfx_gfx_window_s *>(
            GetWindowLongPtr(hwnd, GWLP_USERDATA));

        if (msg == WM_CREATE) {
            self = reinterpret_cast<ysfx_gfx_window_s *>(lparam);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }

        return self ? self->handle_message(hwnd, msg, wparam, lparam) : 0;
    }

    bool create_native()
    {
#if !defined(SWELL_TARGET_GDK) && !defined(__APPLE__)
        // The headless SWELL backend only makes virtual windows; refuse to
        // create one so the host can report "no editor" instead of showing
        // nothing.  Real Linux builds define SWELL_TARGET_GDK; macOS uses
        // the Cocoa SWELL backend.
        YGFX_DBG("create: headless SWELL, no visible window possible\n");
        return false;
#else
        // 0x400001 asks SWELL for a normal top-level, resizable window.
        // 0x400000 asks for a normal top-level, fixed-size window.
        const char *resid = reinterpret_cast<const char *>(
            static_cast<INT_PTR>((desc.flags & ysfx_gfx_window_flag_resizable) ? 0x400001 : 0x400000));

#ifdef __APPLE__
        ensure_ns_application();
        void *pool = SWELL_InitAutoRelease();
#endif
        hwnd = SWELL_CreateDialog(nullptr, resid, nullptr,
                                  reinterpret_cast<DLGPROC>(&ysfx_gfx_window_s::window_proc),
                                  reinterpret_cast<LPARAM>(this));
#ifdef __APPLE__
        if (pool)
            SWELL_QuitAutoRelease(pool);
#endif

        if (!hwnd)
            return false;

        SWELL_SetClassName(hwnd, kWindowClass);
        SWELL_EnableMetal(hwnd, 1);
        SetWindowText(hwnd, title.c_str());
        SetWindowLong(hwnd, GWL_EXSTYLE,
                      GetWindowLong(hwnd, GWL_EXSTYLE) | WS_EX_ACCEPTFILES);

        int width = clampi(desc.width ? static_cast<int>(desc.width) : 640, 16, 8192);
        int height = clampi(desc.height ? static_cast<int>(desc.height) : 480, 16, 8192);
        resize_native_client(width, height);

        cursor = LoadCursor(nullptr, reinterpret_cast<const char *>(static_cast<INT_PTR>(32512)));
        return true;
#endif
    }

    void destroy_native()
    {
        if (hwnd) {
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }
    }

    void set_native_title()
    {
        if (hwnd)
            SetWindowText(hwnd, title.c_str());
    }

    void resize_native_client(int width, int height)
    {
        if (!hwnd)
            return;

        RECT wr{}, cr{};
        GetWindowRect(hwnd, &wr);
        GetClientRect(hwnd, &cr);

        int frame_w = (wr.right - wr.left) - (cr.right - cr.left);
        int frame_h = (wr.bottom - wr.top) - (cr.bottom - cr.top);

        SetWindowPos(hwnd, nullptr, 0, 0,
                     width + frame_w, height + frame_h,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
#endif

    // ── framebuffer / gfx setup ───────────────────────────────────────────

    bool ensure_framebuffer()
    {
        if (!hwnd)
            return false;

        RECT r{};
        GetClientRect(hwnd, &r);
        client_width = static_cast<uint32_t>(clampi(r.right - r.left, 1, 16384));
        client_height = static_cast<uint32_t>(clampi(r.bottom - r.top, 1, 16384));

        if (!wants_retina)
            pixel_scale = 1.0;

        uint32_t new_width = static_cast<uint32_t>(clampi(
            static_cast<int>(std::lround(client_width * pixel_scale)), 1, 16384));
        uint32_t new_height = static_cast<uint32_t>(clampi(
            static_cast<int>(std::lround(client_height * pixel_scale)), 1, 16384));

        if (framebuffer && fb_width == new_width && fb_height == new_height)
            return framebuffer->getBits() != nullptr;

        framebuffer.reset(new LICE_SysBitmap(static_cast<int>(new_width), static_cast<int>(new_height)));
        fb_width = new_width;
        fb_height = new_height;

        if (!framebuffer->getBits()) {
            framebuffer.reset();
            fb_width = 0;
            fb_height = 0;
            return false;
        }

        need_setup = true;
        force_paint = true;
        return true;
    }

    void setup_gfx()
    {
        if (!fx || !framebuffer || !framebuffer->getBits())
            return;

        ysfx_gfx_config_t gc{};
        gc.user_data = this;
        gc.pixel_width = fb_width;
        gc.pixel_height = fb_height;
        gc.pixel_stride = static_cast<uint32_t>(framebuffer->getRowSpan()) * sizeof(LICE_pixel);
        gc.pixels = reinterpret_cast<uint8_t *>(framebuffer->getBits());
        gc.scale_factor = pixel_scale;
        gc.show_menu = &ysfx_gfx_window_s::show_menu_callback;
        gc.set_cursor = &ysfx_gfx_window_s::set_cursor_callback;
        gc.get_drop_file = &ysfx_gfx_window_s::get_drop_file_callback;

        ysfx_gfx_setup(fx, &gc);
        need_setup = false;
    }

    // ── input ─────────────────────────────────────────────────────────────

    void update_mouse_from_cursor()
    {
        if (!hwnd)
            return;

        POINT p{};
        GetCursorPos(&p);
        ScreenToClient(hwnd, &p);

        RECT r{};
        GetClientRect(hwnd, &r);

        bool inside = p.x >= 0 && p.y >= 0 && p.x < r.right && p.y < r.bottom;
        if (inside) {
            mouse_client_x = p.x;
            mouse_client_y = p.y;
        }

        mouse_over = inside || mouse_buttons != 0;
    }

    void update_mouse_state()
    {
        if (!fx)
            return;

        int x = 0;
        int y = 0;

        if (client_width > 0 && client_height > 0) {
            x = static_cast<int>(std::lround(
                static_cast<double>(mouse_client_x) * fb_width / client_width));
            y = static_cast<int>(std::lround(
                static_cast<double>(mouse_client_y) * fb_height / client_height));
        }

        ysfx_gfx_update_mouse(fx, current_mods(), x, y, mouse_buttons, wheel, hwheel);
    }

    void update_window_state()
    {
        if (!fx || !hwnd)
            return;

        bool focused = GetFocus() == hwnd;
        bool visible = IsWindowVisible(hwnd) != 0;
        ysfx_gfx_set_window_state(fx, focused, visible, mouse_over);
    }

    void reset_wheel()
    {
        wheel = 0.0;
        hwheel = 0.0;
    }

    // ── callbacks from ysfx ──────────────────────────────────────────────

    static int32_t show_menu_callback(void *user_data, const char *menu_spec,
                                      int32_t xpos, int32_t ypos)
    {
        auto *self = static_cast<ysfx_gfx_window_s *>(user_data);
        return self ? self->show_menu(menu_spec, xpos, ypos) : 0;
    }

    int32_t show_menu(const char *menu_spec, int32_t xpos, int32_t ypos)
    {
        if (!hwnd || !menu_spec)
            return 0;

        ysfx_menu_t *parsed = ysfx_parse_menu(menu_spec);
        if (!parsed)
            return 0;

        HMENU menu = CreatePopupMenu();
        if (!menu) {
            ysfx_menu_free(parsed);
            return 0;
        }

        uint32_t index = 0;
        int position = 0;
        bool ok = build_menu_items(menu, parsed, index, position);

        int32_t result = 0;
        if (ok) {
            int cx = client_width > 0
                ? static_cast<int>(std::lround(
                      static_cast<double>(xpos) * client_width / fb_width))
                : xpos;
            int cy = client_height > 0
                ? static_cast<int>(std::lround(
                      static_cast<double>(ypos) * client_height / fb_height))
                : ypos;

            POINT pt{cx, cy};
            ClientToScreen(hwnd, &pt);

            result = TrackPopupMenu(menu,
                                    TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY,
                                    pt.x, pt.y, 0, hwnd, nullptr);
        }

        DestroyMenu(menu);
        ysfx_menu_free(parsed);
        return result;
    }

    static void set_cursor_callback(void *user_data, int32_t cursor)
    {
        auto *self = static_cast<ysfx_gfx_window_s *>(user_data);
        if (self)
            self->set_cursor(cursor);
    }

    void set_cursor(int32_t cursor_id)
    {
        cursor_resid = cursor_id;

#ifdef _WIN32
        cursor = cursor_id > 0 ? LoadCursorW(nullptr, MAKEINTRESOURCEW(cursor_id)) : nullptr;
#else
        cursor = cursor_id > 0
            ? LoadCursor(nullptr, reinterpret_cast<const char *>(static_cast<INT_PTR>(cursor_id)))
            : nullptr;
#endif

        if (cursor && mouse_over)
            SetCursor(cursor);
    }

    static const char *get_drop_file_callback(void *user_data, int32_t index)
    {
        auto *self = static_cast<ysfx_gfx_window_s *>(user_data);
        return self ? self->get_drop_file(index) : nullptr;
    }

    const char *get_drop_file(int32_t index)
    {
        if (index == -1) {
            drop_files.clear();
            return nullptr;
        }

        if (index < 0 || static_cast<size_t>(index) >= drop_files.size())
            return nullptr;

        return drop_files[static_cast<size_t>(index)].c_str();
    }

    void handle_drop_files(HDROP drop)
    {
        drop_files.clear();

#ifdef _WIN32
        UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            UINT length = DragQueryFileW(drop, i, nullptr, 0);
            if (length == 0)
                continue;

            std::wstring path(length, L'\0');
            DragQueryFileW(drop, i, path.data(), length + 1);

            int utf8_length = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (utf8_length > 1) {
                std::string utf8(static_cast<size_t>(utf8_length - 1), '\0');
                WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8.data(), utf8_length, nullptr, nullptr);
                drop_files.push_back(std::move(utf8));
            }
        }
#else
        UINT count = DragQueryFile(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            UINT length = DragQueryFile(drop, i, nullptr, 0);
            if (length == 0)
                continue;

            std::string path(length + 1, '\0');
            DragQueryFile(drop, i, path.data(), length + 1);
            path.resize(std::strlen(path.c_str()));
            drop_files.push_back(std::move(path));
        }
#endif

        DragFinish(drop);
    }

    void remember_key(uint32_t vk, uint32_t key)
    {
        for (auto it = key_map.begin(); it != key_map.end(); ++it) {
            if (it->first == vk) {
                key_map.erase(it);
                break;
            }
        }

        key_map.emplace_back(vk, key);
    }

    void handle_key_down(uint32_t vk)
    {
        last_vk = vk;

        uint32_t key = 0;
        if (translate_special_key(vk, key)) {
            ysfx_gfx_add_key(fx, current_mods(), key, true);
            remember_key(vk, key);
            suppress_char_vk = vk;
            return;
        }

#ifdef _WIN32
        // Ctrl/Alt + A..Z does not always generate WM_CHAR.  Send it here and
        // suppress the duplicate character message.
        uint32_t mods = current_mods();
        if ((mods & (ysfx_mod_ctrl | ysfx_mod_alt)) && vk >= 'A' && vk <= 'Z') {
            key = static_cast<uint32_t>('a' + (vk - 'A'));
            ysfx_gfx_add_key(fx, mods, key, true);
            remember_key(vk, key);
            suppress_char_vk = vk;
        }
#else
        // SWELL only synthesizes WM_KEYDOWN/WM_KEYUP; unlike Win32 there is no
        // TranslateMessage and thus no WM_CHAR.  Derive the character here.
        uint32_t character = vk;

#ifndef _WIN32
        // SWELL marks the numeric keypad enter with bit 15.
        if (character == (32768u | VK_RETURN))
            character = VK_RETURN;
#endif

#ifdef __APPLE__
        // keyDown hands us charactersIgnoringModifiers, which loses the shift
        // level for symbols.  Ask SWELL for the fully translated character.
        int flags = 0;
        int mac = SWELL_MacKeyToWindowsKeyEx(nullptr, &flags, 1);
        (void)flags;
        if (mac > 0 && mac <= 0xff)
            character = static_cast<uint32_t>(mac);
#endif

        if (character >= 'A' && character <= 'Z' &&
            !(current_mods() & ysfx_mod_shift))
            character = static_cast<uint32_t>('a' + (character - 'A'));

        // Tab (9), Return (13) and printable Latin-1 characters.
        if (character == 9 || character == 13 || character >= 32)
            handle_text_input(character);
#endif
    }

    void handle_key_up(uint32_t vk)
    {
        for (size_t i = key_map.size(); i > 0; --i) {
            if (key_map[i - 1].first == vk) {
                ysfx_gfx_add_key(fx, current_mods(), key_map[i - 1].second, false);
                key_map.erase(key_map.begin() + static_cast<std::ptrdiff_t>(i - 1));
                break;
            }
        }

        if (suppress_char_vk == vk)
            suppress_char_vk = 0;

        if (last_vk == vk)
            last_vk = 0;
    }

    void handle_text_input(uint32_t character)
    {
        if (!fx || character == 0)
            return;

        // JSFX only understands Latin-1 characters.
        if (character > 0xff)
            return;

        uint32_t mods = current_mods();

        // Windows sends Ctrl+A as character 1, Ctrl+Z as 26.  ysfx's key map
        // expects the letter itself, matching the JUCE graphics view.
        if ((mods & ysfx_mod_ctrl) && character >= 1 && character <= 26)
            character = static_cast<uint32_t>('a' + (character - 1));

        ysfx_gfx_add_key(fx, mods, character, true);

        if (last_vk != 0)
            remember_key(last_vk, character);
    }

    void handle_char(uint32_t character)
    {
        if (!fx || character == 0)
            return;

        if (suppress_char_vk != 0) {
            suppress_char_vk = 0;
            return;
        }

        handle_text_input(character);
    }

    // ── messages ─────────────────────────────────────────────────────────

    LRESULT handle_message(HWND h, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        switch (msg) {
        case WM_CREATE:
            hwnd = h;
            open = true;
            return 0;

        case WM_CLOSE:
            DestroyWindow(h);
            return 0;

        case WM_DESTROY:
            open = false;
            hwnd = nullptr;
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(h, &ps);

            if (dc) {
                RECT r{};
                GetClientRect(h, &r);
                int cw = r.right - r.left;
                int ch = r.bottom - r.top;

                if (framebuffer && framebuffer->getDC() && fb_width > 0 && fb_height > 0) {
                    HDC src = framebuffer->getDC();
                    if (cw == static_cast<int>(fb_width) && ch == static_cast<int>(fb_height)) {
                        BitBlt(dc, 0, 0, cw, ch, src, 0, 0, SRCCOPY);
                    } else {
#ifdef _WIN32
                        SetStretchBltMode(dc, HALFTONE);
#endif
                        StretchBlt(dc, 0, 0, cw, ch, src, 0, 0,
                                   static_cast<int>(fb_width), static_cast<int>(fb_height),
                                   SRCCOPY);
                    }
                } else {
#ifdef _WIN32
                    HBRUSH brush = CreateSolidBrush(RGB(0, 0, 0));
                    FillRect(dc, &r, brush);
                    DeleteObject(brush);
#else
                    HBRUSH brush = CreateSolidBrush(RGB(0, 0, 0));
                    FillRect(dc, &r, brush);
                    DeleteObject(brush);
#endif
                }

                EndPaint(h, &ps);
            }
            return 0;
        }

        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED) {
                if (ensure_framebuffer()) {
                    force_paint = true;
                    InvalidateRect(h, nullptr, FALSE);
                }
            }
            return 0;

        case WM_GETMINMAXINFO: {
            auto *mmi = reinterpret_cast<MINMAXINFO *>(lparam);
            mmi->ptMinTrackSize.x = 32;
            mmi->ptMinTrackSize.y = 32;
            return 0;
        }

        case WM_MOUSEMOVE:
            mouse_client_x = static_cast<int>(static_cast<short>(LOWORD(lparam)));
            mouse_client_y = static_cast<int>(static_cast<short>(HIWORD(lparam)));
            mouse_over = true;
            return 0;

        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            mouse_buttons |= ysfx_button_left;
            SetFocus(h);
            SetCapture(h);
            return 0;
        case WM_LBUTTONUP:
            mouse_buttons &= ~ysfx_button_left;
            if (mouse_buttons == 0)
                ReleaseCapture();
            return 0;

        case WM_MBUTTONDOWN:
        case WM_MBUTTONDBLCLK:
            mouse_buttons |= ysfx_button_middle;
            SetFocus(h);
            SetCapture(h);
            return 0;
        case WM_MBUTTONUP:
            mouse_buttons &= ~ysfx_button_middle;
            if (mouse_buttons == 0)
                ReleaseCapture();
            return 0;

        case WM_RBUTTONDOWN:
        case WM_RBUTTONDBLCLK:
            mouse_buttons |= ysfx_button_right;
            SetFocus(h);
            SetCapture(h);
            return 0;
        case WM_RBUTTONUP:
            mouse_buttons &= ~ysfx_button_right;
            if (mouse_buttons == 0)
                ReleaseCapture();
            return 0;

        case WM_MOUSEWHEEL: {
            POINT p{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
            ScreenToClient(h, &p);
            mouse_client_x = p.x;
            mouse_client_y = p.y;
            mouse_over = true;
            wheel += static_cast<short>(HIWORD(wparam)) / 120.0;
            return 0;
        }

        case WM_MOUSEHWHEEL: {
            POINT p{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
            ScreenToClient(h, &p);
            mouse_client_x = p.x;
            mouse_client_y = p.y;
            mouse_over = true;
            hwheel += static_cast<short>(HIWORD(wparam)) / 120.0;
            return 0;
        }

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            handle_key_down(static_cast<uint32_t>(wparam));
            return 0;

        case WM_KEYUP:
        case WM_SYSKEYUP:
            handle_key_up(static_cast<uint32_t>(wparam));
            return 0;

        case WM_CHAR:
        case WM_SYSCHAR:
            handle_char(static_cast<uint32_t>(wparam));
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT && cursor) {
                SetCursor(cursor);
                return TRUE;
            }
            break;

        case WM_DROPFILES:
            handle_drop_files(reinterpret_cast<HDROP>(wparam));
            return 0;

        case WM_SHOWWINDOW:
            if (wparam == 0)
                mouse_over = false;
            return 0;

        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            return 0;

        default:
            break;
        }

#ifdef _WIN32
        return DefWindowProcW(h, msg, wparam, lparam);
#else
        return 0;
#endif
    }

    // ── event loop ───────────────────────────────────────────────────────

    void process_events()
    {
        if (close_requested.load(std::memory_order_relaxed) && hwnd && !close_posted) {
            close_posted = true;
            if (!PostMessage(hwnd, WM_CLOSE, 0, 0))
                DestroyWindow(hwnd);
        }

#ifdef _WIN32
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                open = false;
                break;
            }

            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
#else
        SWELL_RunEvents();
        SWELL_MessageQueue_Flush();
#endif
    }

    void render()
    {
        if (!fx || !open || !hwnd)
            return;

        if (need_setup || !framebuffer)
            ensure_framebuffer();

        if (need_setup)
            setup_gfx();

        if (!framebuffer || !framebuffer->getBits())
            return;

        update_mouse_from_cursor();
        update_window_state();
        update_mouse_state();

        bool modified = ysfx_gfx_run(fx);
        reset_wheel();

        if (modified || force_paint) {
            force_paint = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            UpdateWindow(hwnd);
        }
    }

    bool pump(int32_t timeout_ms)
    {
        if (!open || !hwnd)
            return false;

        process_events();

        if (!open || !hwnd)
            return false;

        auto now = std::chrono::steady_clock::now();
        if (now >= next_render) {
            render();

            uint32_t fps = ysfx_get_requested_framerate(fx);
            if (fps == 0)
                fps = 30;
            fps = static_cast<uint32_t>(clampi(static_cast<int>(fps), 1, 240));

            next_render = now + std::chrono::milliseconds(1000 / fps);
        }

        if (timeout_ms > 0) {
            auto until_frame = std::chrono::duration_cast<std::chrono::milliseconds>(
                next_render - std::chrono::steady_clock::now()).count();
            int sleep_ms = static_cast<int>(clampd(
                static_cast<double>(until_frame), 0.0, static_cast<double>(timeout_ms)));
            if (sleep_ms > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        }

        return open && hwnd;
    }

    void request_close()
    {
        close_requested.store(true, std::memory_order_relaxed);
    }

    void show_native(bool show)
    {
        if (hwnd)
            ShowWindow(hwnd, show ? SW_SHOW : SW_HIDE);
    }

    void resize_client(int width, int height)
    {
        width = clampi(width, 16, 8192);
        height = clampi(height, 16, 8192);

#ifdef _WIN32
        if (!hwnd)
            return;

        DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
        RECT rect{0, 0, width, height};
        AdjustWindowRectEx(&rect, style, FALSE, WS_EX_ACCEPTFILES);
        SetWindowPos(hwnd, nullptr, 0, 0,
                     rect.right - rect.left, rect.bottom - rect.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
#else
        resize_native_client(width, height);
#endif
    }

    void set_always_on_top(bool on_top)
    {
        if (!hwnd)
            return;

#ifdef _WIN32
        SetWindowPos(hwnd, on_top ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#else
        // SWELL has no HWND_TOPMOST.  The caller can still bring the window to
        // the front through the native handle if the platform needs it.
        (void)on_top;
#endif
    }
};

#endif // !YSFX_NO_GFX

// ── C ABI ─────────────────────────────────────────────────────────────────

extern "C" {

ysfx_gfx_window_t *ysfx_gfx_window_create(ysfx_t *fx, const ysfx_gfx_window_desc_t *desc)
{
#if !defined(YSFX_NO_GFX)
    if (!fx || !ysfx_has_section(fx, ysfx_section_gfx)) {
        YGFX_DBG("create: effect has no @gfx section\n");
        return nullptr;
    }

    auto *window = new (std::nothrow) ysfx_gfx_window_s(fx, desc);
    if (!window) {
        YGFX_DBG("create: out of memory\n");
        return nullptr;
    }

    if (!window->create_native()) {
        YGFX_DBG("create: native window failed\n");
        window->destroy_native();
        delete window;
        return nullptr;
    }

    if (!window->ensure_framebuffer()) {
        YGFX_DBG("create: framebuffer failed\n");
        window->destroy_native();
        delete window;
        return nullptr;
    }

    YGFX_DBG("create: ok\n");

    window->set_always_on_top((window->desc.flags & ysfx_gfx_window_flag_always_on_top) != 0);
    window->show_native(true);

    return window;
#else
    (void)fx;
    (void)desc;
    return nullptr;
#endif
}

void ysfx_gfx_window_destroy(ysfx_gfx_window_t *window)
{
    if (!window)
        return;

#if !defined(YSFX_NO_GFX)
    window->destroy_native();
    delete window;
#else
    (void)window;
#endif
}

bool ysfx_gfx_window_is_open(const ysfx_gfx_window_t *window)
{
#if !defined(YSFX_NO_GFX)
    return window && window->open && window->hwnd != nullptr;
#else
    (void)window;
    return false;
#endif
}

bool ysfx_gfx_window_pump(ysfx_gfx_window_t *window, int32_t timeout_ms)
{
#if !defined(YSFX_NO_GFX)
    return window && window->pump(timeout_ms);
#else
    (void)window;
    (void)timeout_ms;
    return false;
#endif
}

int32_t ysfx_gfx_window_run(ysfx_gfx_window_t *window)
{
#if !defined(YSFX_NO_GFX)
    if (!window)
        return -1;

    while (window->pump(5)) {
    }

    return 0;
#else
    (void)window;
    return -1;
#endif
}

void ysfx_gfx_window_request_close(ysfx_gfx_window_t *window)
{
#if !defined(YSFX_NO_GFX)
    if (window)
        window->request_close();
#else
    (void)window;
#endif
}

void ysfx_gfx_window_set_title(ysfx_gfx_window_t *window, const char *title)
{
#if !defined(YSFX_NO_GFX)
    if (!window)
        return;

    window->title = title ? title : "";
    window->set_native_title();
#else
    (void)window;
    (void)title;
#endif
}

void ysfx_gfx_window_resize(ysfx_gfx_window_t *window, uint32_t width, uint32_t height)
{
#if !defined(YSFX_NO_GFX)
    if (window)
        window->resize_client(static_cast<int>(width), static_cast<int>(height));
#else
    (void)window;
    (void)width;
    (void)height;
#endif
}

void ysfx_gfx_window_show(ysfx_gfx_window_t *window, bool show)
{
#if !defined(YSFX_NO_GFX)
    if (window)
        window->show_native(show);
#else
    (void)window;
    (void)show;
#endif
}

void *ysfx_gfx_window_get_native_handle(const ysfx_gfx_window_t *window)
{
#if !defined(YSFX_NO_GFX)
    return window ? reinterpret_cast<void *>(window->hwnd) : nullptr;
#else
    (void)window;
    return nullptr;
#endif
}

} // extern "C"
