#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <atomic>
#include <algorithm>

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"
#include <kthook/kthook.hpp>

ImFont* combo_arrow = nullptr;

using PresentFn = HRESULT(__stdcall*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using ResetFn   = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

static PresentFn g_oPresent = nullptr;
static ResetFn   g_oReset   = nullptr;
static WNDPROC   g_oWndProc = nullptr;
static HWND      g_hwnd     = nullptr;

static void hide_d3d_cursor(IDirect3DDevice9* dev) {
    if (!dev) return;
    __try {
        void** vt = *reinterpret_cast<void***>(dev);
        if (IsBadReadPtr(vt, sizeof(void*) * 18)) return;
        using Fn = BOOL(__stdcall*)(IDirect3DDevice9*, BOOL);
        Fn fn = reinterpret_cast<Fn>(vt[12]);
        if (!fn || IsBadCodePtr((FARPROC)fn)) return;
        fn(dev, FALSE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static std::atomic<bool> g_open{false};
static bool g_autosave = true;
static bool g_imguiInit = false;
static bool g_cmdDone = false;
static uint64_t g_savedSig = 0;
static int g_frame = 0;
static int g_openMode = 0;
static char g_cmd[32] = "/xpt";
static int g_bindKey = 0x2D;
static bool g_listening = false;
static DWORD g_listenTick = 0;

using CmdProcFn = void(__cdecl*)(const char*);
using AddCmdFn = void(__thiscall*)(void*, const char*, CmdProcFn);
static bool CleanCmd(const char* src, char* dst);

static void __cdecl CmdXpt(const char* params) {
    (void)params;
    g_open = !g_open.load();
}

static bool TryOneCmd(uint32_t inOff, uint32_t addOff, const char* cmd) {
    __try {
        HMODULE samp = GetModuleHandleA("samp.dll");
        if (!samp) return false;
        uint8_t* base = reinterpret_cast<uint8_t*>(samp);
        void* input = *reinterpret_cast<void**>(base + inOff);
        if (!input) return false;
        if (IsBadReadPtr(input, 0x1500)) return false;
        int cnt = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(input) + 0x14DC);
        if (cnt < 0 || cnt >= 144) return false;

        for (int i = 0; i < cnt; ++i) {
            char* name = reinterpret_cast<char*>(reinterpret_cast<uint8_t*>(input) + 0x24C + (size_t)i * 33);
            if (IsBadReadPtr(name, 4)) continue;
            if (_stricmp(name, cmd) == 0) return true;
        }
        void* fn = base + addOff;
        if (IsBadReadPtr(fn, 16)) return false;
        if (IsBadCodePtr((FARPROC)fn)) return false;
        reinterpret_cast<AddCmdFn>(fn)(input, cmd, &CmdXpt);

        if (IsBadReadPtr(input, 0x1500)) return false;
        int cnt2 = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(input) + 0x14DC);
        if (cnt2 <= cnt || cnt2 > 144) {

            for (int i = 0; i < cnt; ++i) {
                char* name = reinterpret_cast<char*>(reinterpret_cast<uint8_t*>(input) + 0x24C + (size_t)i * 33);
                if (IsBadReadPtr(name, 4)) continue;
                if (_stricmp(name, cmd) == 0) return true;
            }
            return false;
        }
        for (int i = 0; i < cnt2; ++i) {
            char* name = reinterpret_cast<char*>(reinterpret_cast<uint8_t*>(input) + 0x24C + (size_t)i * 33);
            if (IsBadReadPtr(name, 4)) continue;
            if (_stricmp(name, cmd) == 0) return true;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void SafeTryRegister() {

    static const uint32_t kCand[][2] = {
        {0x26E8CC, 0x69000},
        {0x21A0E8, 0x65AD0},
        {0x21A0F0, 0x65BA0},
        {0x26E9FC, 0x69730},
        {0x26E9FC, 0x69770},
        {0x26EB84, 0x69770},
        {0x2ACA14, 0x691B0},
    };
    char cmd[32];
    if (!CleanCmd(g_cmd, cmd)) return;
    for (size_t k = 0; k < sizeof(kCand) / sizeof(kCand[0]); ++k) {
        if (TryOneCmd(kCand[k][0], kCand[k][1], cmd)) { g_cmdDone = true; return; }
    }
}

struct Weapon {
    int id, slot, base, vmin, vmax;
    int val;
    int mult;
    bool x;
    bool on;
    uint32_t last;
    bool has;
    bool edit;
    bool editFocus;
};
static Weapon g_ak{78, 4, 120, 120, 777, 360, 3, true, true, 0, false, false, false};
static Weapon g_uzi{28, 4, 100, 100, 1000, 300, 3, true, true, 0, false, false, false};
static Weapon g_ob{26, 3, 4, 4, 777, 12, 3, true, true, 0, false, false, false};
static void ClampWeapon(Weapon& w) {
    if (w.val < w.vmin) w.val = w.vmin;
    if (w.val > w.vmax) w.val = w.vmax;
    if (w.mult < 1) w.mult = 1;
    if (w.mult > 30) w.mult = 30;
}
static int TargetOf(Weapon& w) {
    return w.x ? w.base * w.mult : w.val;
}

static DWORD GetCfgDword(HKEY k, const char* name, DWORD def) {
    DWORD v = def;
    DWORD s = sizeof(v);
    DWORD t = 0;
    if (RegQueryValueExA(k, name, nullptr, &t, reinterpret_cast<BYTE*>(&v), &s) != ERROR_SUCCESS || t != REG_DWORD) return def;
    return v;
}
static void GetCfgStr(HKEY k, const char* name, char* out, int n, const char* def) {
    size_t i = 0;
    while (def[i] && i + 1 < (size_t)n) { out[i] = def[i]; ++i; }
    out[i] = 0;
    DWORD t = 0;
    DWORD s = (DWORD)n;
    if (RegQueryValueExA(k, name, nullptr, &t, reinterpret_cast<BYTE*>(out), &s) != ERROR_SUCCESS || t != REG_SZ) {
        i = 0;
        while (def[i] && i + 1 < (size_t)n) { out[i] = def[i]; ++i; }
        out[i] = 0;
        return;
    }
    out[n - 1] = 0;
}
static bool CleanCmd(const char* src, char* dst) {
    int n = 0;
    for (; *src && n < 31; ++src) {
        char c = *src;
        if (c == '/' && n == 0) continue;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') dst[n++] = c;
        else return false;
    }
    dst[n] = 0;
    return n > 0;
}
static void KeyName(int vk, char* out, int n) {
    out[0] = 0;
    UINT sc = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    if (sc) {
        LONG l = (LONG)(sc << 16);
        if (vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END ||
            vk == VK_PRIOR || vk == VK_NEXT || vk == VK_LEFT || vk == VK_UP ||
            vk == VK_RIGHT || vk == VK_DOWN || vk == VK_NUMLOCK ||
            vk == VK_RMENU || vk == VK_RCONTROL || vk == VK_DIVIDE) l |= 0x1000000;
        if (GetKeyNameTextA(l, out, n) > 0 && out[0]) return;
    }
    _snprintf_s(out, (size_t)n, (size_t)n, "VK %02X", vk);
}
static void LoadCfg() {
    HKEY k = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\xpt", 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    int m;
    m = (int)GetCfgDword(k, "val_ak", 360); g_ak.val = m;
    m = (int)GetCfgDword(k, "m_ak", 3); g_ak.mult = m; ClampWeapon(g_ak);
    m = (int)GetCfgDword(k, "val_uzi", 300); g_uzi.val = m;
    m = (int)GetCfgDword(k, "m_uzi", 3); g_uzi.mult = m; ClampWeapon(g_uzi);
    m = (int)GetCfgDword(k, "val_ob", 12); g_ob.val = m;
    m = (int)GetCfgDword(k, "m_ob", 3); g_ob.mult = m; ClampWeapon(g_ob);
    g_ak.x = GetCfgDword(k, "x_ak", 1) != 0;
    g_uzi.x = GetCfgDword(k, "x_uzi", 1) != 0;
    g_ob.x = GetCfgDword(k, "x_ob", 1) != 0;
    g_ak.on = GetCfgDword(k, "en_ak", 1) != 0;
    g_uzi.on = GetCfgDword(k, "en_uzi", 1) != 0;
    g_ob.on = GetCfgDword(k, "en_ob", 1) != 0;
    g_autosave = GetCfgDword(k, "autosave", 1) != 0;
    g_openMode = GetCfgDword(k, "openmode", 0) ? 1 : 0;
    g_bindKey = (int)GetCfgDword(k, "bindkey", 0x2D);
    if (g_bindKey < 1 || g_bindKey > 254) g_bindKey = 0x2D;
    GetCfgStr(k, "cmdname", g_cmd, sizeof(g_cmd), "/xpt");
    {
        char tmp[32];
        if (CleanCmd(g_cmd, tmp)) {
            g_cmd[0] = '/';
            size_t i = 0;
            while (tmp[i] && i + 1 < sizeof(g_cmd) - 1) { g_cmd[1 + i] = tmp[i]; ++i; }
            g_cmd[1 + i] = 0;
        } else {
            g_cmd[0] = '/'; g_cmd[1] = 'x'; g_cmd[2] = 'p'; g_cmd[3] = 't'; g_cmd[4] = 0;
        }
    }
    RegCloseKey(k);
}
static void SaveCfg() {
    HKEY k = nullptr;
    DWORD d = 0;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\xpt", 0, nullptr, 0, KEY_WRITE, nullptr, &k, &d) != ERROR_SUCCESS) return;
    auto put = [&](const char* name, DWORD v) {
        RegSetValueExA(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
    };
    put("val_ak", (DWORD)g_ak.val); put("m_ak", (DWORD)g_ak.mult); put("x_ak", g_ak.x ? 1 : 0); put("en_ak", g_ak.on ? 1 : 0);
    put("val_uzi", (DWORD)g_uzi.val); put("m_uzi", (DWORD)g_uzi.mult); put("x_uzi", g_uzi.x ? 1 : 0); put("en_uzi", g_uzi.on ? 1 : 0);
    put("val_ob", (DWORD)g_ob.val); put("m_ob", (DWORD)g_ob.mult); put("x_ob", g_ob.x ? 1 : 0); put("en_ob", g_ob.on ? 1 : 0);
    put("autosave", g_autosave ? 1 : 0);
    put("openmode", (DWORD)g_openMode);
    put("bindkey", (DWORD)g_bindKey);
    RegSetValueExA(k, "cmdname", 0, REG_SZ, reinterpret_cast<const BYTE*>(g_cmd), (DWORD)strlen(g_cmd) + 1);
    RegCloseKey(k);
}
static uint64_t Sig() {
    uint64_t s = (uint64_t)(g_ak.val & 1023);
    s |= (uint64_t)(g_ak.mult & 31) << 10;
    s |= (uint64_t)(g_ak.x ? 1 : 0) << 15;
    s |= (uint64_t)(g_ak.on ? 1 : 0) << 16;
    s |= (uint64_t)(g_uzi.val & 1023) << 17;
    s |= (uint64_t)(g_uzi.mult & 31) << 27;
    s |= (uint64_t)(g_uzi.x ? 1 : 0) << 32;
    s |= (uint64_t)(g_uzi.on ? 1 : 0) << 33;
    s |= (uint64_t)(g_ob.val & 1023) << 34;
    s |= (uint64_t)(g_ob.mult & 31) << 44;
    s |= (uint64_t)(g_ob.x ? 1 : 0) << 49;
    s |= (uint64_t)(g_ob.on ? 1 : 0) << 50;
    s |= (uint64_t)(g_autosave ? 1 : 0) << 51;
    {
        uint32_t h = 5381;
        for (const char* p = g_cmd; *p; ++p) h = h * 33 + (unsigned char)*p;
        uint64_t o = (uint64_t)(g_openMode & 1);
        o |= (uint64_t)(g_bindKey & 255) << 1;
        o |= (uint64_t)(h & 511) << 9;
        s |= o << 52;
    }
    return s;
}

static void ApplyCmd(const char* tmp) {
    char name[32];
    if (!CleanCmd(tmp, name)) return;
    char with[34];
    with[0] = '/';
    size_t i = 0;
    while (name[i] && i < 30) { with[1 + i] = name[i]; ++i; }
    with[1 + i] = 0;
    if (_stricmp(g_cmd, with) != 0) {
        size_t j = 0;
        while (with[j] && j < 31) { g_cmd[j] = with[j]; ++j; }
        g_cmd[j] = 0;
        g_cmdDone = false;
        if (g_autosave) SaveCfg();
    }
}

static void ProcessWeapon(Weapon& w) {
    __try {
        if (!w.on) { w.has = false; return; }
        uint32_t ped = *reinterpret_cast<uint32_t*>(0xB6F5F0);
        if (!ped) { w.has = false; return; }
        if (IsBadReadPtr(reinterpret_cast<void*>(ped), 0x800)) { w.has = false; return; }
        uint8_t cur = *reinterpret_cast<uint8_t*>(ped + 0x718);
        if (cur > 12) { w.has = false; return; }
        uint32_t wbase = ped + 0x5A0 + cur * 0x1C;
        if (IsBadReadPtr(reinterpret_cast<void*>(wbase), 0x1C)) { w.has = false; return; }
        if (*reinterpret_cast<uint32_t*>(wbase) != (uint32_t)w.id) { w.has = false; return; }
        if (IsBadWritePtr(reinterpret_cast<void*>(wbase + 0x8), sizeof(uint32_t))) { w.has = false; return; }
        uint32_t target = (uint32_t)TargetOf(w);
        uint32_t ammo = *reinterpret_cast<uint32_t*>(wbase + 0x8);
        if (!w.has) {
            if (ammo > 0 && ammo < target) { *reinterpret_cast<uint32_t*>(wbase + 0x8) = target; ammo = target; }
        } else if (ammo > w.last) {
            if (ammo < target) { *reinterpret_cast<uint32_t*>(wbase + 0x8) = target; ammo = target; }
        }
        w.last = ammo;
        w.has = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        w.has = false;
    }
}

static void WeaponTab(const char* tab, const char* sid, const char* msid, Weapon& w) {
    if (ImGui::BeginTabItem(tab)) {
        if (ImGui::Checkbox("enable", &w.on)) { w.has = false; if (g_autosave) SaveCfg(); }
        if (ImGui::Checkbox("x", &w.x)) { w.has = false; w.edit = false; if (g_autosave) SaveCfg(); }
        if (w.x) {
            char sb[32];
            _snprintf_s(sb, sizeof(sb), "x##%s", msid);
            if (ImGui::SliderInt(sb, &w.mult, 1, 30, "")) { ClampWeapon(w); w.has = false; if (g_autosave) SaveCfg(); }
        } else {
            char sb[32];
            _snprintf_s(sb, sizeof(sb), "x##%s", sid);
            if (ImGui::SliderInt(sb, &w.val, w.vmin, w.vmax, "")) { ClampWeapon(w); w.has = false; if (g_autosave) SaveCfg(); }
        }
        if (!w.edit) {
            ImGui::Text("pt: %d", TargetOf(w));
            if (ImGui::IsItemClicked()) { w.edit = true; w.editFocus = true; }
        } else {
            char ib[32];
            _snprintf_s(ib, sizeof(ib), "##in_%s", sid);
            if (w.editFocus) { ImGui::SetKeyboardFocusHere(); w.editFocus = false; }
            if (ImGui::InputInt(ib, &w.val, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) {
                ClampWeapon(w); w.has = false; w.x = false; if (g_autosave) SaveCfg(); w.edit = false;
            } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                ClampWeapon(w); w.edit = false;
            } else if (ImGui::IsItemDeactivated()) {
                ClampWeapon(w); w.has = false; w.x = false; if (g_autosave) SaveCfg(); w.edit = false;
            }
        }
        ImGui::EndTabItem();
    }
}

static void DrawMenu() {
    ImGui::SetNextWindowSize(ImVec2(300, 230), ImGuiCond_FirstUseEver);
    bool open = true;
    ImGui::Begin("xpt", &open);
    if (ImGui::BeginTabBar("weapons")) {
        WeaponTab("ak47", "ak", "mak", g_ak);
        WeaponTab("uzi", "uzi", "muzi", g_uzi);
        WeaponTab("obrez", "ob", "mob", g_ob);
        if (ImGui::BeginTabItem("option")) {
            if (!combo_arrow && ImGui::GetIO().Fonts->Fonts.Size > 0) combo_arrow = ImGui::GetIO().Fonts->Fonts[0];
            const char* modes[] = { "cmd", "keybind" };
            if (ImGui::Combo("open", &g_openMode, modes, 2)) { if (g_autosave) SaveCfg(); }
            if (g_openMode == 0) {
                char tmp[32];
                size_t i = 0;
                while (g_cmd[i] && i < 31) { tmp[i] = g_cmd[i]; ++i; }
                tmp[i] = 0;
                if (ImGui::InputText("cmd", tmp, sizeof(tmp), ImGuiInputTextFlags_EnterReturnsTrue)) ApplyCmd(tmp);
                else if (ImGui::IsItemDeactivatedAfterEdit()) ApplyCmd(tmp);
            } else {
                char kb[64];
                KeyName(g_bindKey, kb, sizeof(kb));
                if (g_listening) {
                    if (ImGui::Button("press key...")) g_listening = false;
                } else {
                    if (ImGui::Button(kb)) { g_listening = true; g_listenTick = GetTickCount(); }
                }
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (ImGui::Checkbox("cfg", &g_autosave)) SaveCfg();
    ImGui::End();
    if (!open) g_open = false;
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

using SetCursorPosFn = BOOL(WINAPI*)(int, int);
static kthook::kthook_simple<SetCursorPosFn> g_setCursorHook{};
static bool g_setHookDone = false;

static void normalize_cursor_hidden() {
    int c = ShowCursor(FALSE);
    if (c >= 0) { while (ShowCursor(FALSE) >= 0) {} }
    else if (c < -1) { while (ShowCursor(TRUE) < -1) {} }
}

static int hkSetCursorPos(const decltype(g_setCursorHook)& hook, int& x, int& y) {
    if (g_open.load()) return TRUE;
    return hook.get_trampoline()(x, y);
}

static LRESULT __stdcall hkWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    __try {
        if (g_open.load()) {
            if (m == WM_KEYDOWN && w == VK_ESCAPE) {
                if (!ImGui::GetIO().WantTextInput) { g_open = false; return 1; }
            }
        }
        if (g_open.load() && g_imguiInit) {
            ImGui_ImplWin32_WndProcHandler(h, m, w, l);
            if (ImGui::GetIO().WantCaptureMouse || ImGui::GetIO().WantCaptureKeyboard) {
                switch (m) {
                    case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR: case WM_UNICHAR:
                    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
                    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_XBUTTONDOWN: case WM_XBUTTONUP:
                    case WM_MOUSEMOVE: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: case WM_INPUT:
                        return 1;
                }
            }
            if ((m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || (m >= WM_KEYFIRST && m <= WM_KEYLAST))
                return 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (g_oWndProc) return CallWindowProcA(g_oWndProc, h, m, w, l);
    return DefWindowProcA(h, m, w, l);
}

static void DoImguiInit(IDirect3DDevice9* dev) {
    __try {
        HWND hwnd = nullptr;
        if (!IsBadReadPtr(reinterpret_cast<void*>(0xC17054), sizeof(HWND*))) {
            HWND* pp = *reinterpret_cast<HWND**>(0xC17054);
            if (pp && !IsBadReadPtr(pp, sizeof(HWND))) hwnd = *pp;
        }
        if (!hwnd || !IsWindow(hwnd)) hwnd = GetForegroundWindow();
        g_hwnd = hwnd;
        ImGui::CreateContext();
        ImGui_ImplWin32_Init(hwnd);
        ImGui_ImplDX9_Init(dev);
        ImGui::GetIO().IniFilename = nullptr;

        if (hwnd && IsWindow(hwnd)) {
            g_oWndProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&hkWndProc)));
            if (!g_oWndProc) g_oWndProc = nullptr;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void ZeroPads() {
    *reinterpret_cast<short*>(0xB73458 + 0x08) = 0;
    *reinterpret_cast<short*>(0xB73458 + 0x22) = 0;
    *reinterpret_cast<short*>(0xB73458 + 0x1A) = 0;
    *reinterpret_cast<short*>(0xB73458 + 0x0A) = 0;
    memset(reinterpret_cast<void*>(0xB73404), 0, 0x14);
    memset(reinterpret_cast<void*>(0xB73418), 0, 0x14);
    memset(reinterpret_cast<void*>(0xB7342C), 0, 0x14);
}

static void DoFrame(IDirect3DDevice9* dev) {
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ImGui::GetIO().MouseDrawCursor = g_open.load();

    static int d3dGrace = 0;
    if (g_open.load()) { d3dGrace = 0; hide_d3d_cursor(dev); }
    else if (d3dGrace < 30) { ++d3dGrace; hide_d3d_cursor(dev); }
    static int closeFrames = 0;
    if (g_open.load()) { closeFrames = 0; normalize_cursor_hidden(); }
    else if (closeFrames < 90) { ++closeFrames; normalize_cursor_hidden(); }

    if (g_open.load() && !g_setHookDone) {
        HMODULE u32 = GetModuleHandleA("user32.dll");
        if (u32) {
            g_setCursorHook.set_dest(GetProcAddress(u32, "SetCursorPos"));
            g_setCursorHook.set_cb(&hkSetCursorPos);
            g_setCursorHook.install();
            g_setHookDone = true;
        }
    }

    if (g_open.load()) ZeroPads();

    ProcessWeapon(g_ak);
    ProcessWeapon(g_uzi);
    ProcessWeapon(g_ob);
    if (g_open.load()) DrawMenu();
    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

static void SafeDoFrame(IDirect3DDevice9* dev) {
    __try {
        DoFrame(dev);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void ScanBind() {
    if (GetTickCount() - g_listenTick < 300) return;
    for (int vk = 1; vk < 255; ++vk) {
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN ||
            vk == VK_CAPITAL || vk == VK_NUMLOCK || vk == VK_SCROLL) continue;
        if (GetAsyncKeyState(vk) & 0x8000) {
            g_bindKey = vk;
            g_listening = false;
            if (g_autosave) SaveCfg();
            break;
        }
    }
}

static HRESULT __stdcall hkPresent(IDirect3DDevice9* dev,
                  const RECT* s, const RECT* d, HWND wd, const RGNDATA* r) {
    if (!g_imguiInit) {
        DoImguiInit(dev);
        g_imguiInit = true;
    }

    if (!g_cmdDone) {
        static int tries = 0;
        if (++tries > 60) { tries = 0; SafeTryRegister(); }
    }

    if (g_listening) ScanBind();

    if (g_openMode == 1 && !g_listening) {
        static bool prevKey = false;
        bool curKey = (GetAsyncKeyState(g_bindKey) & 0x8000) != 0;
        if (curKey && !prevKey && !ImGui::GetIO().WantTextInput) g_open = !g_open.load();
        prevKey = curKey;
    }

    SafeDoFrame(dev);

    if (++g_frame > 300) {
        g_frame = 0;
        if (g_autosave) {
            uint64_t n = Sig();
            if (n != g_savedSig) { g_savedSig = n; SaveCfg(); }
        }
    }
    if (g_oPresent) return g_oPresent(dev, s, d, wd, r);
    return D3D_OK;
}

static HRESULT __stdcall hkReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    __try { ImGui_ImplDX9_InvalidateDeviceObjects(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    HRESULT hr = D3D_OK;
    if (g_oReset) {
        __try { hr = g_oReset(dev, pp); } __except (EXCEPTION_EXECUTE_HANDLER) { hr = D3D_OK; }
    }
    __try { ImGui_ImplDX9_CreateDeviceObjects(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return hr;
}

static bool PatchVTable(IDirect3DDevice9* dev) {
    __try {
        void** vt = *reinterpret_cast<void***>(dev);
        if (!vt || IsBadReadPtr(vt, sizeof(void*) * 18)) return false;

        DWORD old = 0;

        if (!g_oPresent) {
            if (IsBadReadPtr(&vt[17], sizeof(void*))) return false;
            g_oPresent = reinterpret_cast<PresentFn>(vt[17]);
            if (!g_oPresent || IsBadCodePtr((FARPROC)g_oPresent)) { g_oPresent = nullptr; return false; }
            if (!VirtualProtect(&vt[17], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
            vt[17] = reinterpret_cast<void*>(&hkPresent);
            VirtualProtect(&vt[17], sizeof(void*), old, &old);
        }
        if (!g_oReset) {
            if (IsBadReadPtr(&vt[16], sizeof(void*))) return false;
            g_oReset = reinterpret_cast<ResetFn>(vt[16]);
            if (!g_oReset || IsBadCodePtr((FARPROC)g_oReset)) { g_oReset = nullptr; return false; }
            if (!VirtualProtect(&vt[16], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
            vt[16] = reinterpret_cast<void*>(&hkReset);
            VirtualProtect(&vt[16], sizeof(void*), old, &old);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static IDirect3DDevice9* WaitDevice() {
    IDirect3DDevice9* dev = nullptr;
    for (int i = 0; i < 300 && !dev; ++i) {
        __try { dev = *reinterpret_cast<IDirect3DDevice9**>(0xC97C28); }
        __except (EXCEPTION_EXECUTE_HANDLER) { dev = nullptr; }
        if (!dev) Sleep(200);
    }
    return dev;
}

static DWORD WINAPI ZeroThread(LPVOID) {
    for (;;) {
        if (g_open.load()) {
            __try { ZeroPads(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        Sleep(1);
    }
    return 0;
}

static DWORD WINAPI InitThread(LPVOID) {
    LoadCfg();
    g_savedSig = Sig();
    CloseHandle(CreateThread(nullptr, 0, &ZeroThread, nullptr, 0, nullptr));
    IDirect3DDevice9* dev = WaitDevice();
    if (!dev) return 0;

    Sleep(1000);
    for (int i = 0; i < 10; ++i) {
        if (PatchVTable(dev)) break;
        Sleep(500);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(GetModuleHandleA(nullptr));
        CloseHandle(CreateThread(nullptr, 0, &InitThread, nullptr, 0, nullptr));
    }
    return TRUE;
}
