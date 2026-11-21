// MGPU Bridge - panel_simple: the one-click panel for people who just want to play.
//
// WHAT THIS IS. A second view of the SAME controls the full panel has, with the log lines, the
// counters and the timing readouts stripped out, so a player who does not care about any of that can
// change what matters and get on with the game. It is NOT a mock-up: every row here calls the same
// setter the full panel calls (ui_nrs_stage / ui_nrs_set / ...), so a change applies live, to the
// same state, and persists the same way. If a row is added here it must carry real behaviour - the
// whole point of the mode is that it is the full panel with less on screen, not a simplified copy.
//
// THE LAYOUT IS DATA, NOT CODE. `panel_simple.txt` next to the add-on decides which rows appear and
// in what order, and carries their Chinese labels and their help text. A row that exists in code but
// not in the file does not appear; a file line whose id has no drawer is reported and skipped. So a
// new feature lands by adding one `row()` call and one line in the file - the simple panel never has
// to be "unfrozen" for it.
//
// CHINESE. The add-on cannot add a font to ReShade's ImGui (it links only through the function
// table, which exposes no ImFontAtlas entry point - see the LNK2019 note in dllmain). The glyphs
// therefore have to come from the font ReShade itself loaded: `Font=` in ReShade.ini. When that font
// has no CJK glyphs the panel falls back to the English label automatically, so a player who never
// touches ReShade.ini still gets a usable panel instead of boxes.
//
// Log tag: [R330] (init / file, mode switch, unknown id). R330 is the merge of N01950's commit;
// every later squashed commit on this work is a letter on it: R330, R330, ... (one letter per
// commit). The merge also carries the review fixes: PanelSimple is held here, not in ui_ini_read's
// 12-slot mirror; the hotkey acts after the bridge thread's message drain (worker.cpp); the full
// panel's install-layout warning is drawn in the simple panel too; "export items" and the
// registered count work before the simple panel was ever drawn.
//
// THREADS (R330, F1). The layout (g_layout, g_by_id, g_rows) is read and rebuilt ONLY on the render
// thread, from the overlay's draw. toggle() - the bridge thread's hotkey - touches the ini only.

#include "panel_simple.hpp"
#include "prism_finish.hpp"   // PRISM-FINISH: 色彩/角色守护 rows drive the module's live atomics
#include "prism_sync.hpp"     // PSYNC: the synchronous path owns its character guard
#include "prism_grade.h"      // the custom-look (Look 5) control table, verbatim from prism-core 1.5.1
#include "t119_keys.hpp"      // PRISM-FINISH: the fps.cap row drives the live FrameCap atomic

#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <atomic>

// Same rule as dllmain: imgui.h has to come first so reshade.hpp binds the ImGui function table, and
// ImTextureID must be ReShade's 64-bit handle. Guarded so a build without the headers still compiles
// (the panel is then absent, as in dllmain's P7.1).
#if defined(__has_include)
#  if __has_include(<imgui.h>)
#    define MGPU_HAVE_IMGUI 1
#    define ImTextureID ImU64
#    include <imgui.h>
#  endif
#endif

#include <reshade.hpp>          // the ImGui wrappers (function table), as everywhere else
#include "gpu1_context.hpp"     // ui_ini_read / ui_ini_write / ui_nrs_* / ui_read
#include "residual.hpp"         // R364: residual::carries for the live Residual / Native row
#include "fg_map.hpp"           // R330: frame-generation mode/keep are live only through fgmap
#include "diag.hpp"

namespace mgpu::psimple
{
    namespace
    {
        struct entry
        {
            std::string id;
            std::string zh;      // Chinese label, empty = use the code's English one
            std::string help;    // \n allowed, expanded when drawn
            // R330: a line that starts with '=' is a SECTION HEADER, not a control. Its text is the
            // title drawn with SeparatorText, and the rows that follow it belong to that section. An
            // empty banner (just '======') draws a plain separator.
            bool        header = false;
        };

        struct row_def
        {
            std::string id;
            std::string en;
            draw_fn     fn = nullptr;
        };

        std::wstring             g_dir;              // the add-on's directory, trailing backslash (R330, F3: wide)
        std::vector<entry>       g_layout;           // as the file has it, in order
        std::vector<row_def>     g_rows;             // registered drawers, in code order
        std::map<std::string, entry> g_by_id;        // lookup for labels/help
        unsigned long long       g_file_mtime = 0;
        // R330: the PanelSimple key, held here. -1 = not read yet; 0 / 1 = the file's value, then
        // whatever set_enabled() last wrote. ui_ini_read is deliberately NOT used: its mirror has 12
        // slots, the full panel already uses most of them, and a full mirror re-reads mgpu.ini every
        // frame for every key it could not cache. Read and written from the file directly instead.
        std::atomic<int>         g_simple{ -1 };

        int simple_flag()
        {
            int v = g_simple.load(std::memory_order_relaxed);
            if (v >= 0) return v;
            char w[16];
            mgpu::gpu1::ui_ini_read_word("PanelSimple", w, sizeof w);   // the file itself, no mirror slot
            v = (w[0] == '1') ? 1 : 0;
            g_simple.store(v, std::memory_order_relaxed);
            return v;
        }
        bool                     g_tried_load = false;
        std::vector<std::string> g_reported;         // ids already complained about

        std::wstring path_of(const wchar_t *name)
        {
            std::wstring p = g_dir;
            p += name;
            return p;
        }

        // For log lines only: the wide path as UTF-8.
        std::string narrow(const std::wstring &w)
        {
            if (w.empty()) return std::string();
            const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
            if (n <= 0) return std::string();
            std::string out((size_t)n, '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
            return out;
        }

        bool read_file(const std::wstring &path, std::string &out)
        {
            FILE *f = nullptr;
            if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr) return false;
            fseek(f, 0, SEEK_END);
            const long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (n <= 0) { fclose(f); out.clear(); return true; }
            out.resize((size_t)n);
            const size_t got = fread(&out[0], 1, (size_t)n, f);
            out.resize(got);
            fclose(f);
            return true;
        }

        unsigned long long mtime_of(const std::wstring &path)
        {
            WIN32_FILE_ATTRIBUTE_DATA d{};
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return 0;
            return ((unsigned long long)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
        }

        std::string trim(const std::string &s)
        {
            size_t a = 0, b = s.size();
            while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
            while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
            return s.substr(a, b - a);
        }

        void parse(const std::string &text)
        {
            g_layout.clear();
            g_by_id.clear();
            size_t pos = 0;
            while (pos <= text.size())
            {
                size_t nl = text.find('\n', pos);
                if (nl == std::string::npos) nl = text.size();
                std::string line = text.substr(pos, nl - pos);
                pos = nl + 1;
                // A UTF-8 BOM on the first line (Notepad writes one) is not a comment and not an id.
                if (line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB &&
                    (unsigned char)line[2] == 0xBF)
                    line.erase(0, 3);
                line = trim(line);
                if (line.empty() || line[0] == '#' || line[0] == ';') continue;
                // R330: a banner is a header. '==== 模型 / 画风 ====' becomes a section title, and an
                // empty '======' becomes a plain separator. The '=' runs are stripped from both ends,
                // so the operator writes the title once, the way the commented banners already read.
                if (line[0] == '=')
                {
                    size_t a = 0, b = line.size();
                    while (a < b && line[a] == '=') ++a;
                    while (b > a && line[b - 1] == '=') --b;
                    entry h;
                    h.header = true;
                    h.id = "section:" + trim(line.substr(a, b - a));
                    h.zh = trim(line.substr(a, b - a));
                    g_layout.push_back(h);
                    continue;
                }
                entry e;
                const size_t p1 = line.find('|');
                if (p1 == std::string::npos)
                {
                    e.id = line;
                }
                else
                {
                    e.id = trim(line.substr(0, p1));
                    const size_t p2 = line.find('|', p1 + 1);
                    if (p2 == std::string::npos)
                        e.zh = trim(line.substr(p1 + 1));
                    else
                    {
                        e.zh = trim(line.substr(p1 + 1, p2 - p1 - 1));
                        e.help = trim(line.substr(p2 + 1));
                    }
                }
                if (e.id.empty()) continue;
                g_by_id[e.id] = e;
                g_layout.push_back(e);
            }
        }

        // R330 (F6): is panel_simple.txt beside the add-on? A stat, no read, no log line.
        bool file_present()
        {
            return !g_dir.empty() && mtime_of(path_of(L"panel_simple.txt")) != 0ull;
        }

        void load_if_changed(bool force)
        {
            if (g_dir.empty()) return;
            const std::wstring p = path_of(L"panel_simple.txt");
            const unsigned long long mt = mtime_of(p);
            if (!force && g_tried_load && mt == g_file_mtime) return;
            g_tried_load = true;
            g_file_mtime = mt;
            std::string text;
            if (!read_file(p, text))
            {
                g_layout.clear();
                g_by_id.clear();
                char l[400];
                snprintf(l, sizeof l, "[MGPU][R330] panel_simple.txt not found at %s - the simple panel will "
                                      "show nothing. Use 'export items' in the full panel to write a template.",
                         narrow(p).c_str());
                mgpu::diag::warn(l);
                return;
            }
            parse(text);
            char l[320];
            snprintf(l, sizeof l, "[MGPU][R330] panel_simple.txt: %u row(s) in the layout (%s)",
                     (unsigned)g_layout.size(), narrow(p).c_str());
            mgpu::diag::info(l);
        }

        const row_def *find_row(const std::string &id)
        {
            for (const row_def &r : g_rows)
                if (r.id == id) return &r;
            return nullptr;
        }

        void report_once(const std::string &id, const char *why)
        {
            for (const std::string &s : g_reported)
                if (s == id) return;
            g_reported.push_back(id);
            char l[320];
            snprintf(l, sizeof l, "[MGPU][R330] panel_simple.txt names '%s' but %s - that line is skipped.",
                     id.c_str(), why);
            mgpu::diag::warn(l);
        }

        // "\n" written as two characters in the file becomes a real line break here.
        std::string unescape(const std::string &s)
        {
            std::string out;
            out.reserve(s.size());
            for (size_t i = 0; i < s.size(); ++i)
            {
                if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') { out.push_back('\n'); ++i; }
                else out.push_back(s[i]);
            }
            return out;
        }
    }   // namespace

    void init(HMODULE mod)
    {
        // R330 (F2): runs inside DllMain, so it only remembers the directory - no file read, no log line.
        // The file is read on the first panel draw that needs it (enabled() with the key set, or the
        // editor section when the file is present).
        wchar_t p[MAX_PATH]{};
        if (GetModuleFileNameW(mod, p, MAX_PATH) == 0) return;
        wchar_t *slash = wcsrchr(p, L'\\');
        if (slash != nullptr) *(slash + 1) = 0;
        g_dir = p;
    }

#ifdef MGPU_HAVE_IMGUI

    bool cjk()
    {
        ImFont *f = ImGui::GetFont();
        return f != nullptr && f->IsGlyphInFont((ImWchar)0x4E2D) != false;
    }

    bool enabled()
    {
        // R330 (F1, F2): called on the render thread only. The key is read first, so with PanelSimple
        // unset nothing is read from disk and nothing is logged.
        if (simple_flag() == 0) return false;   // R330
        load_if_changed(false);
        return !g_layout.empty();
    }

    void set_enabled(bool on)
    {
        // R330: the word writer, not ui_ini_write - the latter would put the key into the mirror.
        (void)mgpu::gpu1::ui_ini_write_word("PanelSimple", on ? "1" : "0");
        g_simple.store(on ? 1 : 0, std::memory_order_relaxed);
        char l[200];
        snprintf(l, sizeof l, "[MGPU][R330] PanelSimple=%d (%s mode)", on ? 1 : 0,
                 on ? "simple" : "full");
        mgpu::diag::info(l);
    }

    void toggle()
    {
        // R330 (F1): the hotkey's path. Flips the flag (R330: read from the file once if nobody has
        // yet) and writes it back; the layout is not touched here, so this is safe from the bridge
        // thread. R330: worker.cpp calls this after its message drain, never inside it.
        set_enabled(simple_flag() == 0);
    }

    void reload() { load_if_changed(true); }
    int layout_count() { return (int)g_layout.size(); }

    void row(const char *id, const char *en, draw_fn fn)
    {
        if (id == nullptr || fn == nullptr) return;
        for (const row_def &r : g_rows)
            if (r.id == id) return;   // registered once
        row_def r;
        r.id = id;
        r.en = (en != nullptr) ? en : id;
        r.fn = fn;
        g_rows.push_back(r);
    }

    const char *zh(const char *id)
    {
        if (id == nullptr) return nullptr;
        auto it = g_by_id.find(id);
        if (it == g_by_id.end() || it->second.zh.empty()) return nullptr;
        return it->second.zh.c_str();
    }

    void label(const char *id, const char *en)
    {
        const char *z = cjk() ? zh(id) : nullptr;
        ImGui::TextUnformatted(z != nullptr ? z : (en != nullptr ? en : ""));
    }

    void help(const char *id)
    {
        auto it = g_by_id.find(id != nullptr ? id : "");
        if (it == g_by_id.end() || it->second.help.empty()) return;
        const std::string body = unescape(it->second.help);
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
        {
            // First line only on hover - the popup carries the whole text.
            const size_t nl = body.find('\n');
            ImGui::SetTooltip("%s", body.substr(0, nl).c_str());
        }
        if (ImGui::IsItemClicked())
            ImGui::OpenPopup(id);
        if (ImGui::BeginPopup(id))
        {
            ImGui::PushTextWrapPos(420.0f);
            ImGui::TextUnformatted(body.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndPopup();
        }
    }

    void open_editor()
    {
        const std::wstring p = path_of(L"panel_simple.txt");
        // notepad is always there; the file's own association is deliberately not used so this works
        // on a machine where .txt is bound to something modal. R330 (F3): the real wide path, quoted.
        const std::wstring arg = L"\"" + p + L"\"";
        ShellExecuteW(nullptr, L"open", L"notepad.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
        mgpu::diag::info("[MGPU][R330] opened panel_simple.txt in notepad");
    }

    namespace { void register_rows(); }   // defined with the rows below

    void export_items()
    {
        register_rows();   // R330: the list exists before the simple panel was ever drawn
        const std::wstring p = path_of(L"panel_simple_items.txt");
        FILE *f = nullptr;
        if (_wfopen_s(&f, p.c_str(), L"wb") != 0 || f == nullptr)
        {
            mgpu::diag::warn("[MGPU][R330] could not write panel_simple_items.txt");
            return;
        }
        // A template the operator can copy lines out of: every registered row with its English name
        // and a placeholder Chinese label, in the file's own syntax.
        fprintf(f, "# Every row this build can put in the simple panel.\n");
        fprintf(f, "# Copy a line into panel_simple.txt and edit the label and the help text.\n");
        fprintf(f, "# format:  id | 中文标签 | 说明（可写 \\n 换行）\n\n");
        for (const row_def &r : g_rows)
            fprintf(f, "%-18s | %s | \n", r.id.c_str(), r.en.c_str());
        fclose(f);
        char l[320];
        snprintf(l, sizeof l, "[MGPU][R330] wrote %u row(s) to %s", (unsigned)g_rows.size(), narrow(p).c_str());
        mgpu::diag::info(l);
    }

    // ---------------------------------------------------------------------
    // R330 (F4): the labels INSIDE the rows (buttons, radio choices, slider text) come from the file
    // like the row labels: a `ui.<name> | label` line, shown when the font has CJK glyphs, English
    // otherwise. A missing line shows the English text. `ui.*` lines are labels, not rows.
    // ---------------------------------------------------------------------
    namespace
    {
        std::string T(const char *key, const char *en)
        {
            const char *z = cjk() ? zh(key) : nullptr;
            return std::string(z != nullptr ? z : en);
        }
        // T + an ImGui id suffix, for button / radio / checkbox labels.
        std::string L(const char *key, const char *en, const char *id)
        {
            std::string s = T(key, en);
            s += "##";
            s += id;
            return s;
        }
        // T made safe as a printf format: a '%' typed in the file is doubled.
        std::string F(const char *key, const char *en)
        {
            const std::string t = T(key, en);
            std::string out;
            out.reserve(t.size() + 4);
            for (char c : t) { out.push_back(c); if (c == '%') out.push_back('%'); }
            return out;
        }
    }

    // ---------------------------------------------------------------------
    // The rows themselves. Each one is the SAME call the full panel makes, with the label removed
    // (the simple panel draws the label from the file): ui_nrs_stage / ui_nrs_set are the live
    // setters, so a drag here is a drag there.
    // ---------------------------------------------------------------------
    namespace
    {
        void row_nrs_scale()
        {
            mgpu::gpu1::ui_state st;
            mgpu::gpu1::ui_read(st);
            if (!st.nrs_on) { ImGui::TextDisabled("NRScale not set in mgpu.ini"); return; }
            float sc = st.nrs_scale;
            const float top = (st.nrs_cap > 1.0f) ? st.nrs_cap : 1.0f;
            ImGui::SetNextItemWidth(190.0f);
            if (ImGui::SliderFloat("##nrs_scale", &sc, 0.30f, top, "%.2f"))
                mgpu::gpu1::ui_nrs_stage(sc);
            ImGui::SameLine();
            if (ImGui::SmallButton(L("ui.default", "default", "nrs_scale").c_str())) mgpu::gpu1::ui_nrs_stage(0.75f);
        }

        void row_nrs_gain()
        {
            mgpu::gpu1::ui_state st;
            mgpu::gpu1::ui_read(st);
            if (!st.nrs_on) { ImGui::TextDisabled("-"); return; }
            float v = st.nrs_gain;
            ImGui::SetNextItemWidth(190.0f);
            if (ImGui::SliderFloat("##nrs_gain", &v, 0.0f, 2.0f, "%.2f"))
                mgpu::gpu1::ui_nrs_set(0, v);
            ImGui::SameLine();
            if (ImGui::SmallButton(L("ui.default", "default", "nrs_gain").c_str())) mgpu::gpu1::ui_nrs_set(0, 1.00f);
        }

        void row_nrs_sharp()
        {
            mgpu::gpu1::ui_state st;
            mgpu::gpu1::ui_read(st);
            if (!st.nrs_on) { ImGui::TextDisabled("-"); return; }
            float v = st.nrs_sharpness;
            ImGui::SetNextItemWidth(190.0f);
            if (ImGui::SliderFloat("##nrs_sharp", &v, 0.0f, 1.0f, "%.2f"))
                mgpu::gpu1::ui_nrs_set(1, v);
            ImGui::SameLine();
            if (ImGui::SmallButton(L("ui.default", "default", "nrs_sharp").c_str())) mgpu::gpu1::ui_nrs_set(1, 0.50f);
        }

        void row_nrs_colour()
        {
            mgpu::gpu1::ui_state st;
            mgpu::gpu1::ui_read(st);
            if (!st.nrs_on) { ImGui::TextDisabled("-"); return; }
            float v = st.nrs_color;
            ImGui::SetNextItemWidth(190.0f);
            if (ImGui::SliderFloat("##nrs_col", &v, 0.0f, 1.0f, "%.2f"))
                mgpu::gpu1::ui_nrs_set(2, v);
            ImGui::SameLine();
            if (ImGui::SmallButton(L("ui.default", "default", "nrs_col").c_str())) mgpu::gpu1::ui_nrs_set(2, 0.90f);
        }

        void row_nrs_mode()
        {
            mgpu::gpu1::ui_state st;
            mgpu::gpu1::ui_read(st);
            if (!st.nrs_on) { ImGui::TextDisabled("-"); return; }
            int mo = (int)st.nrs_mode;
            if (ImGui::RadioButton(L("ui.residual", "residual", "nrs_mode1").c_str(), mo == 1)) mgpu::gpu1::ui_nrs_set(3, 1.0f);
            ImGui::SameLine();
            if (ImGui::RadioButton(L("ui.direct", "direct", "nrs_mode0").c_str(), mo == 0)) mgpu::gpu1::ui_nrs_set(3, 0.0f);
        }

        // ---- R330: the rows a player may also want, all live setters ----
        void row_passes()
        {
            mgpu::gpu1::ui_state st;
            mgpu::gpu1::ui_read(st);
            int p = (int)st.passes;
            if (ImGui::RadioButton("×1##passes", p == 1)) mgpu::gpu1::ui_set_passes(1);
            ImGui::SameLine();
            if (ImGui::RadioButton("×2##passes", p == 2)) mgpu::gpu1::ui_set_passes(2);
        }

        void row_intensity()
        {
            mgpu::gpu1::ui_state st;
            mgpu::gpu1::ui_read(st);
            // PSYNC owns its Compose intensity; do not also amplify NGX's independent intensity.
            const bool sync = mgpu::psync::active();
            float v = sync ? mgpu::psync::intensity() : st.intensity[0];
            ImGui::SetNextItemWidth(190.0f);
            if (ImGui::SliderFloat("##intensity", &v, 0.0f, 3.0f, "%.2f"))
            {
                if (sync)
                {
                    mgpu::psync::set_intensity(v);
                    (void)mgpu::gpu1::ui_ini_write_float("PrismSyncIntensity", v);
                }
                else mgpu::gpu1::ui_set_intensity(0, v);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(L("ui.default", "default", "intensity").c_str()))
            {
                if (sync)
                {
                    mgpu::psync::set_intensity(1.0f);
                    (void)mgpu::gpu1::ui_ini_write_float("PrismSyncIntensity", 1.0f);
                }
                else mgpu::gpu1::ui_set_intensity(0, 2.00f);
            }
        }

        void row_view()
        {
            mgpu::gpu1::ui_state st;
            mgpu::gpu1::ui_read(st);
            int pm = st.present_mode;
            if (ImGui::RadioButton(L("ui.output", "output", "view0").c_str(), pm == 0)) mgpu::gpu1::ui_set_present_mode(0);
            ImGui::SameLine();
            if (ImGui::RadioButton(L("ui.input", "input", "view1").c_str(), pm == 1)) mgpu::gpu1::ui_set_present_mode(1);
            ImGui::SameLine();
            if (ImGui::RadioButton(L("ui.split", "split", "view2").c_str(), pm == 2)) mgpu::gpu1::ui_set_present_mode(2);
            if (pm == 2)
            {
                float sp = st.split_pos;
                ImGui::SetNextItemWidth(190.0f);
                if (ImGui::SliderFloat("##seam", &sp, 0.0f, 1.0f, (F("ui.seam", "seam") + " %.2f").c_str()))
                    mgpu::gpu1::ui_set_split_pos(sp);
            }
        }

        void row_neural()
        {
            mgpu::gpu1::ui_state st;
            mgpu::gpu1::ui_read(st);
            bool on = st.neural;
            if (ImGui::Checkbox("##neural", &on)) mgpu::gpu1::ui_set_neural(on);
        }

        void row_mvec_mode()
        {
            mgpu::gpu1::ui_state st;
            mgpu::gpu1::ui_read(st);
            int m = st.mvec_mode;
            if (ImGui::RadioButton(L("ui.off", "off", "mvecmode0").c_str(), m == 0)) mgpu::gpu1::ui_set_mvec_mode(0);
            ImGui::SameLine();
            if (ImGui::RadioButton(L("ui.synthetic", "synthetic", "mvecmode1").c_str(), m == 1)) mgpu::gpu1::ui_set_mvec_mode(1);
            ImGui::SameLine();
            if (ImGui::RadioButton(L("ui.real", "real", "mvecmode3").c_str(), m == 3)) mgpu::gpu1::ui_set_mvec_mode(3);
        }

        // ---- R330: the model group, the preset curves and the two keys that need a re-arm ----
        void row_model_style()
        {
            mgpu::gpu1::ui_state st;
            mgpu::gpu1::ui_read(st);
            int v = (int)st.style;
            if (ImGui::RadioButton("A##style", v == 0)) mgpu::gpu1::ui_set_tuning_value(3, 0.0f);
            ImGui::SameLine();
            if (ImGui::RadioButton("B##style", v == 1)) mgpu::gpu1::ui_set_tuning_value(3, 1.0f);
            ImGui::SameLine();
            if (ImGui::RadioButton("C##style", v == 2)) mgpu::gpu1::ui_set_tuning_value(3, 2.0f);
        }

        void row_model_tone()
        {
            mgpu::gpu1::ui_state st;
            mgpu::gpu1::ui_read(st);
            float v = st.tone_strength;
            ImGui::SetNextItemWidth(190.0f);
            if (ImGui::SliderFloat("##tone", &v, 0.0f, 2.0f, "%.2f"))
                mgpu::gpu1::ui_set_tuning_value(0, v);
            ImGui::SameLine();
            if (ImGui::SmallButton(L("ui.default", "default", "tone").c_str())) mgpu::gpu1::ui_set_tuning_value(0, 1.00f);
        }

        void row_model_structure()
        {
            mgpu::gpu1::ui_state st;
            mgpu::gpu1::ui_read(st);
            float v = st.structure_strength;
            ImGui::SetNextItemWidth(190.0f);
            if (ImGui::SliderFloat("##structure", &v, 0.0f, 2.0f, "%.2f"))
                mgpu::gpu1::ui_set_tuning_value(1, v);
            ImGui::SameLine();
            if (ImGui::SmallButton(L("ui.default", "default", "structure").c_str())) mgpu::gpu1::ui_set_tuning_value(1, 1.00f);
        }

        void row_model_skin()
        {
            mgpu::gpu1::ui_state st;
            mgpu::gpu1::ui_read(st);
            float v = st.face_mask_strength;
            ImGui::SetNextItemWidth(190.0f);
            if (ImGui::SliderFloat("##facemask", &v, 0.0f, 1.0f, "%.2f"))
                mgpu::gpu1::ui_set_face_mask_strength(v);
            ImGui::SameLine();
            if (ImGui::SmallButton(L("ui.default", "default", "facemask").c_str()))
                mgpu::gpu1::ui_set_face_mask_strength(0.30f);
        }

        void row_preset()
        {
            mgpu::gpu1::ui_state st;
            mgpu::gpu1::ui_read(st);
            int p = st.preset;
            if (ImGui::RadioButton(L("ui.manual", "manual", "preset0").c_str(), p == 0)) mgpu::gpu1::ui_set_preset(0);
            ImGui::SameLine();
            if (ImGui::RadioButton(L("ui.front", "front", "preset1").c_str(), p == 1)) mgpu::gpu1::ui_set_preset(1);
            ImGui::SameLine();
            if (ImGui::RadioButton(L("ui.back", "back", "preset2").c_str(), p == 2)) mgpu::gpu1::ui_set_preset(2);
        }

        // R330: one slider, 0 = off. The checkbox that used to sit here read as "an empty box and a
        // number", which is exactly the ambiguity the simple panel is meant to remove.
        // PRISM-FINISH note: this row now also drives t119's live atomic, like the full panel's
        // Frame pacing section does - a drag here caps this session, not just the next launch.
        void row_frame_cap()
        {
            int v = mgpu::gpu1::ui_ini_read("FrameCap", 0);
            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::SliderInt("##capfps", &v, 0, 240, (v == 0) ? "off" : "%d"))
            {
                mgpu::t119::set_frame_cap(v);
                (void)mgpu::gpu1::ui_ini_write("FrameCap", v);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(L("ui.turn_off", "off", "cap_zero").c_str()))
            {
                mgpu::t119::set_frame_cap(0);
                (void)mgpu::gpu1::ui_ini_write("FrameCap", 0);
            }
        }

        void row_colour16()
        {
            bool on = mgpu::gpu1::ui_ini_read("NRInput16", 0) != 0;
            if (ImGui::Checkbox("##n16", &on))
                (void)mgpu::gpu1::ui_ini_write("NRInput16", on ? 4 : 0);
        }

        void row_crop()
        {
            const int mode = mgpu::gpu1::ui_ini_read("ScaleMode", 0);
            const int auto_crop = mgpu::gpu1::ui_ini_read("CropAuto", 1);
            const int cur = (mode == 0) ? 0 : (auto_crop != 0 ? 2 : 1);
            if (ImGui::RadioButton(L("ui.off", "off", "crop0").c_str(), cur == 0))
                (void)mgpu::gpu1::ui_ini_write("ScaleMode", 0);
            ImGui::SameLine();
            if (ImGui::RadioButton(L("ui.on", "on", "crop1").c_str(), cur == 1))
            {
                (void)mgpu::gpu1::ui_ini_write("CropAuto", 0);
                (void)mgpu::gpu1::ui_ini_write("ScaleMode", 1);
            }
            ImGui::SameLine();
            if (ImGui::RadioButton(L("ui.auto", "auto", "crop2").c_str(), cur == 2))
            {
                (void)mgpu::gpu1::ui_ini_write("CropAuto", 1);
                (void)mgpu::gpu1::ui_ini_write("ScaleMode", 1);
            }
        }

        // ---- R330: the blocks that only exist on titles with DLSS, frame generation, or x2 ----
        // The bridge is not an FC6 add-on. On a title with native DLSS these are the controls a
        // player actually touches, so they belong in the simple panel too - and every one of them is
        // marked for what it is: live, or "needs a restart".
        void row_sr_enable()
        {
            bool on = mgpu::gpu1::ui_ini_read("SRUpscale", 0) != 0;
            if (ImGui::Checkbox("##sron", &on))
                (void)mgpu::gpu1::ui_ini_write("SRUpscale", on ? 1 : 0);
        }

        void row_sr_quality()
        {
            // R330 (F5): the SAME writes as the full panel's DLSS box (dllmain, write_mode): SRQuality
            // always; SRScale=0 + SRMvLowRes=0 when Native Upscaling is on (SRScale=0 in the ini), else
            // SRScale 67/58/50 + SRMvLowRes=1. SRUpscale is not written here (that is sr.enable). Like the
            // full panel, the choice is greyed while DLSS on GPU 1 is off or Native Upscaling is on.
            const bool on    = mgpu::gpu1::ui_ini_read("SRUpscale", 0) != 0;
            const bool match = mgpu::gpu1::ui_ini_read("SRScale", 0) == 0;
            const int  mode  = mgpu::gpu1::ui_ini_read("SRQuality", 2);
            const auto write_mode = [match](int m)
            {
                (void)mgpu::gpu1::ui_ini_write("SRQuality", m);
                if (match)
                {
                    (void)mgpu::gpu1::ui_ini_write("SRScale",    0);
                    (void)mgpu::gpu1::ui_ini_write("SRMvLowRes", 0);
                }
                else
                {
                    const int scale = (m == 2) ? 67 : ((m == 1) ? 58 : 50);
                    (void)mgpu::gpu1::ui_ini_write("SRScale",    scale);
                    (void)mgpu::gpu1::ui_ini_write("SRMvLowRes", 1);
                }
            };
            ImGui::BeginDisabled(!on || match);
            if (ImGui::RadioButton(L("ui.performance", "performance", "srq0").c_str(), mode == 0)) write_mode(0);
            ImGui::SameLine();
            if (ImGui::RadioButton(L("ui.balanced", "balanced", "srq1").c_str(), mode == 1)) write_mode(1);
            ImGui::SameLine();
            if (ImGui::RadioButton(L("ui.quality", "quality", "srq2").c_str(), mode == 2)) write_mode(2);
            ImGui::EndDisabled();
            ImGui::SameLine();
        }

        void row_sr_preset()
        {
            const int pr = mgpu::gpu1::ui_ini_read("SRPreset", 0);
            if (ImGui::RadioButton(L("ui.title_default", "title default", "srp0").c_str(), pr == 0))       (void)mgpu::gpu1::ui_ini_write("SRPreset", 0);
            ImGui::SameLine();
            if (ImGui::RadioButton("K##srp", pr == 11))          (void)mgpu::gpu1::ui_ini_write("SRPreset", 11);
            ImGui::SameLine();
            if (ImGui::RadioButton("L##srp", pr == 12))          (void)mgpu::gpu1::ui_ini_write("SRPreset", 12);
            ImGui::SameLine();
            if (ImGui::RadioButton("M##srp", pr == 13))          (void)mgpu::gpu1::ui_ini_write("SRPreset", 13);
        }

        void row_fg_mode()
        {
            const int fm = (int)mgpu::fgmap::ui_mode();
            if (ImGui::RadioButton(L("ui.off", "off", "fg0").c_str(), fm == 0))
            { mgpu::fgmap::ui_set_mode(0); (void)mgpu::gpu1::ui_ini_write("NRFrameFilter", 0); }
            ImGui::SameLine();
            if (ImGui::RadioButton(L("ui.fg_fix", "fix vectors", "fg2").c_str(), fm == 2))
            { mgpu::fgmap::ui_set_mode(2); (void)mgpu::gpu1::ui_ini_write("NRFrameFilter", 2); }
            ImGui::SameLine();
            if (ImGui::RadioButton(L("ui.fg_fix_thin", "fix + thin", "fg3").c_str(), fm == 3))
            { mgpu::fgmap::ui_set_mode(3); (void)mgpu::gpu1::ui_ini_write("NRFrameFilter", 3); }
        }

        void row_fg_keep()
        {
            int n = (int)mgpu::fgmap::ui_keep();
            ImGui::SetNextItemWidth(110.0f);
            if (ImGui::SliderInt("##fgkeep", &n, 2, 8, "%d"))
                mgpu::fgmap::ui_set_keep((unsigned)n);
            if (ImGui::IsItemDeactivatedAfterEdit())
                (void)mgpu::gpu1::ui_ini_write("NRFrameKeep", (int)mgpu::fgmap::ui_keep());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", T("ui.fg_keep_hint", "keep 1 in N").c_str());
        }

        void row_x2_mode()
        {
            // Same three writes the full panel makes: Full is ResidualPass=0; Medium and Light are
            // ResidualPass=2 plus a 0.75 scale, differing only in ResidualRender.
            const int rp = mgpu::gpu1::ui_ini_read("ResidualPass", 0);
            const int rr = mgpu::gpu1::ui_ini_read("ResidualRender", 0);
            const int cur = (rp == 0) ? 2 : (rr == 1 ? 1 : 0);   // 0 Medium, 1 Light, 2 Full
            if (ImGui::RadioButton(L("ui.medium", "medium", "x2_0").c_str(), cur == 0))
            {
                (void)mgpu::gpu1::ui_ini_write("ResidualPass", 2);
                (void)mgpu::gpu1::ui_ini_write_float("ResidualScale", 0.75f);
                (void)mgpu::gpu1::ui_ini_write("ResidualRender", 0);
            }
            ImGui::SameLine();
            if (ImGui::RadioButton(L("ui.light", "light", "x2_1").c_str(), cur == 1))
            {
                (void)mgpu::gpu1::ui_ini_write("ResidualPass", 2);
                (void)mgpu::gpu1::ui_ini_write_float("ResidualScale", 0.75f);
                (void)mgpu::gpu1::ui_ini_write("ResidualRender", 1);
            }
            ImGui::SameLine();
            if (ImGui::RadioButton(L("ui.full", "full", "x2_2").c_str(), cur == 2))
                (void)mgpu::gpu1::ui_ini_write("ResidualPass", 0);
            ImGui::SameLine();
        }

        void row_nrs1()
        {
            char v[32] = {};
            mgpu::gpu1::ui_ini_read_word("NRScale", v, sizeof v);
            const bool on = (v[0] != 0 && atof(v) > 0.05);
            if (ImGui::RadioButton("off##nrs1", !on))
                (void)mgpu::gpu1::ui_ini_write_float("NRScale", 0.0f);
            ImGui::SameLine();
            if (ImGui::RadioButton("on##nrs1", on))
            {
                (void)mgpu::gpu1::ui_ini_write_float("NRScale", 0.75f);
                (void)mgpu::gpu1::ui_ini_write("NRScaleResolveDepth", 0);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("pass 1 at 0.75, detail added back at full size (restart)");
        }

        // ---- PRISM-FINISH rows (2026-10-29 pivot): prism-core 1.5.1's 色彩 / 角色守护 ----
        // The setters are live atomics read by the very next dispatch; the ini writes persist the
        // value for the next arm. Rows are disabled text unless the module armed (Finish=1).
        void row_finish_on()
        {
            const bool on = mgpu::pfinish::active();
            if (ImGui::RadioButton("off##finish", !on))
                (void)mgpu::gpu1::ui_ini_write("Finish", 0);
            ImGui::SameLine();
            if (ImGui::RadioButton("on##finish", on))
                (void)mgpu::gpu1::ui_ini_write("Finish", 1);
            ImGui::SameLine();
            ImGui::TextDisabled("prism grade + guard (restart)");
        }

        void row_finish_look()
        {
            if (!mgpu::pfinish::active()) { ImGui::TextDisabled("Finish=1 not set in mgpu.ini"); return; }
            static const char *names[5] = { "\xE9\xBB\x98\xE8\xAE\xA4", "\xE9\xAB\x98\xE8\xBF\x98\xE5\x8E\x9F",
                                            "\xE9\xAB\x98\xE9\xA5\xB1\xE5\x92\x8C", "\xE4\xBD\x8E\xE9\xA5\xB1\xE5\x92\x8C",
                                            "\xE6\xB5\x93\xE7\x83\x88" };   // 默认/高还原/高饱和/低饱和/浓烈 (UTF-8, file is /utf-8)
            int v = (int)mgpu::pfinish::look();
            ImGui::SetNextItemWidth(190.0f);
            if (ImGui::Combo("##finish_look", &v, names, 5))
            {
                mgpu::pfinish::set_look((unsigned)v);
                (void)mgpu::gpu1::ui_ini_write("FinishLook", v);
            }
        }

        void row_finish_guard()
        {
            const bool sync = mgpu::psync::active();
            if (!sync && !mgpu::pfinish::active()) { ImGui::TextDisabled("-"); return; }
            float v = sync ? mgpu::psync::guard() : mgpu::pfinish::guard();
            ImGui::SetNextItemWidth(190.0f);
            if (ImGui::SliderFloat("##finish_guard", &v, 0.0f, 1.0f, "%.2f"))
            {
                if (sync)
                {
                    mgpu::psync::set_guard(v);
                    (void)mgpu::gpu1::ui_ini_write_float("PrismSyncGuard", v);
                }
                else
                {
                    mgpu::pfinish::set_guard(v);
                    (void)mgpu::gpu1::ui_ini_write_float("FinishGuard", v);
                }
            }
        }

        // ---- R356 (0.5.0, Marcelo's shape, the same keys the full panel's "Pass settings" writes) ----
        // Network: Light-FP8 (the small network, NRPreset<n>=0, green, FPS+) | Full-FP8 (the big one,
        // NRPreset<n>=1, red, FPS-). Resolution: Residual Neural (green, FPS+) | Native (red, FPS-) -
        // pass 1 is NRS 1|0; pass 2 is Pass2 nrs|full with NRS=1, ResidualPass 2|0 without it.
        // All read at arm: the restart line is the full panel's.
        bool ps_pick(const std::string &label, bool selected, bool green)
        {
            const ImVec4 on  = green ? ImVec4(0.18f, 0.62f, 0.30f, 1.0f) : ImVec4(0.72f, 0.24f, 0.24f, 1.0f);
            const ImVec4 off = green ? ImVec4(0.18f, 0.62f, 0.30f, 0.28f) : ImVec4(0.72f, 0.24f, 0.24f, 0.28f);
            ImGui::PushStyleColor(ImGuiCol_Button, selected ? on : off);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, on);
            const bool c = ImGui::Button(label.c_str(), ImVec2(140.0f, 0.0f));
            ImGui::PopStyleColor(3);
            return c;
        }
        void ps_net(unsigned i)
        {
            static const char *const key[4] = { "NRPreset1", "NRPreset2", "NRPreset3", "NRPreset4" };   // literals: the mirror keeps the pointer; arm reads NRPreset<u> per pass
            if (i >= 4u) return;
            // R363: live. The row shows the network the pass runs now; a click rebuilds it (ui_set_network) and
            // writes the key for the next launch (pass 1 at x2+ -> NRPreset1x2).
            mgpu::gpu1::ui_state stn; mgpu::gpu1::ui_read(stn);
            const bool live = stn.armed && stn.nr_ok;
            const char *key_now = (i == 0u && stn.passes >= 2u) ? "NRPreset1x2" : key[i];
            const int cur = live ? ((stn.nr_preset_pass[i] == 1) ? 1 : 0)
                                 : ((mgpu::gpu1::ui_ini_read(key[i], mgpu::gpu1::ui_ini_read("NRPreset", 0)) == 1) ? 1 : 0);
            char id[16];
            snprintf(id, sizeof id, "psn%u0", i);
            if (ps_pick(L("ui.light_fp8", "Light-FP8  FPS+", id), cur == 0, true) && cur != 0)
            { (void)mgpu::gpu1::ui_ini_write(key_now, 0); if (live) mgpu::gpu1::ui_set_network(i, 0); }   // R364: ini first
            ImGui::SameLine();
            snprintf(id, sizeof id, "psn%u1", i);
            if (ps_pick(L("ui.full_fp8", "Full-FP8  FPS-", id), cur == 1, false) && cur != 1)
            { (void)mgpu::gpu1::ui_ini_write(key_now, 1); if (live) mgpu::gpu1::ui_set_network(i, 1); }
            ImGui::SameLine();
        }
        void ps_res(unsigned i)
        {
            const bool nrs1 = (mgpu::gpu1::ui_ini_read("NRS", 0) == 1);
            const int rp = mgpu::gpu1::ui_ini_read("ResidualPass", 0);
            mgpu::gpu1::ui_state stn; mgpu::gpu1::ui_read(stn);
            const bool live = stn.armed && stn.nr_ok;   // R364: the row shows and switches what the pass runs now
            int cur;
            if (live) cur = mgpu::residual::carries(i) ? 1 : 0;
            else if (i == 0u) cur = nrs1 ? 1 : 0;
            else if (i == 1u)
            {
                char w[16] = {};
                mgpu::gpu1::ui_ini_read_word("Pass2", w, sizeof w);
                cur = (nrs1 && w[0] != 0) ? ((w[0] == 'n' || w[0] == 'N') ? 1 : 0) : (rp == 2 ? 1 : 0);
            }
            else cur = 0;   // pass 3+: no arm-time key exists; live shows it, ini has nothing to mirror
            auto write = [&](int v)
            {
                if (i == 0u) (void)mgpu::gpu1::ui_ini_write("NRS", v);
                else if (i == 1u)
                {
                    if (nrs1) (void)mgpu::gpu1::ui_ini_write_word("Pass2", v ? "nrs" : "full");
                    else      (void)mgpu::gpu1::ui_ini_write("ResidualPass", v ? 2 : 0);
                }
                // pass 3+: live-only, the arm reads no key for them
                if (live) mgpu::gpu1::ui_set_resolution(i, v == 1);   // R364: after the ini write
            };
            char id[16];
            snprintf(id, sizeof id, "psr%u1", i);
            if (ps_pick(L("ui.residual_neural", "Residual Neural  FPS+", id), cur == 1, true) && cur != 1) write(1);
            ImGui::SameLine();
            snprintf(id, sizeof id, "psr%u0", i);
            if (ps_pick(L("ui.native", "Native  FPS-", id), cur == 0, false) && cur != 0) write(0);
            ImGui::SameLine();
        }
        void row_pass1_net() { ps_net(0u); }
        void row_pass1_res() { ps_res(0u); }
        void row_pass2_net() { ps_net(1u); }
        void row_pass2_res() { ps_res(1u); }
        // row_fg_gpu1 deliberately omitted: FrameGen is not built in this branch.
        void row_pass_best()
        {
            if (ImGui::Button(L("ui.best_qp", "Best Quality-Performance", "psbest").c_str()))
            {
                // Marcelo's default (R360): Full-FP8 + Residual on pass 1 and on the last (2nd) pass.
                (void)mgpu::gpu1::ui_ini_write("NRS", 1);
                (void)mgpu::gpu1::ui_ini_write("NRPreset1", 1);
                (void)mgpu::gpu1::ui_ini_write("NRPreset1x2", 0);   // R361: Light-FP8 on pass 1 while x2 runs
                (void)mgpu::gpu1::ui_ini_write("NRPreset2", 1);
                (void)mgpu::gpu1::ui_ini_write_word("Pass2", "nrs");
            }
            ImGui::SameLine();
        }

        void register_rows()
        {
            row("nrs.scale",  "NRScale",        &row_nrs_scale);
            row("nrs.gain",   "detail gain",    &row_nrs_gain);
            row("nrs.sharp",  "sharpness",      &row_nrs_sharp);
            row("nrs.colour", "colour strength",&row_nrs_colour);
            row("nrs.mode",   "mode",           &row_nrs_mode);
            // R330: nrs1.on is the author's "Pass 1 scale (NRS)" On/Off radio, nrs.scale is the live
            // slider under "Neural scale (our own path)". BOTH already exist in 0.4.0 and both write
            // NRScale, so they are one control: the simple panel exposes a single slider and accepts
            // either id in the layout file for it.
            row("nrs1.on",    "pass 1 scale",   &row_nrs_scale);
            row("passes",     "passes",         &row_passes);
            row("intensity",  "intensity",      &row_intensity);
            row("intensity.preset", "intensity curve", &row_preset);
            row("view",       "view",           &row_view);
            row("neural",     "neural stage",   &row_neural);
            row("mvec.mode",  "motion vectors", &row_mvec_mode);
            row("model.style",     "model style",  &row_model_style);
            row("model.tone",      "tone",         &row_model_tone);
            row("model.structure", "structure",    &row_model_structure);
            row("model.skin",      "face mask",    &row_model_skin);
            row("fps.cap",         "fps cap",      &row_frame_cap);
            row("colour.16bit",    "16-bit colour",&row_colour16);
            row("crop",            "crop",         &row_crop);
            // R330: these seven were written but never registered, so the DLSS / frame-generation /
            // x2 sections came up empty. A row has to be BOTH drawn and registered.
            row("x2.mode",         "2nd pass (x2)",&row_x2_mode);
            row("pass1.network",   "pass 1 network",    &row_pass1_net);   // R356
            row("pass1.resolution","pass 1 resolution", &row_pass1_res);
            row("pass2.network",   "pass 2 network",    &row_pass2_net);
            row("pass2.resolution","pass 2 resolution", &row_pass2_res);
            row("pass.best",       "defaults",          &row_pass_best);
            row("sr.enable",       "DLSS on GPU 1",&row_sr_enable);
            row("sr.quality",      "DLSS quality", &row_sr_quality);
            row("sr.preset",       "DLSS preset",  &row_sr_preset);
            row("fg.mode",         "frame generation", &row_fg_mode);
            row("fg.keep",         "keep 1 in N",  &row_fg_keep);
            // PRISM-FINISH rows
            row("finish.on",       "prism finish", &row_finish_on);
            row("finish.look",     "色彩",         &row_finish_look);
            row("finish.guard",    "角色守护",     &row_finish_guard);
        }
    }   // namespace

    // ---- prism-style widgets (2026-10-29 pivot): the panel is drawn in prism-core 1.5.1's
    // layout language - label in a left column, segmented control or slider on the right, dark
    // chrome, a frame-time bar chart on top. The setters are the same ones the data-driven rows
    // used; only the drawing changed.
    namespace
    {
        const char *T_(const char *z, const char *e) { return cjk() ? z : e; }

        // prism's control table labels are wide chars; the panel string is UTF-8.
        std::string w2u(const wchar_t *w)
        {
            if (w == nullptr) return std::string();
            const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
            if (n <= 1) return std::string();
            std::string s((size_t)n - 1u, '\0');
            WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
            return s;
        }

        // prism palette (prism DESIGN.md): a black field, white ink, 1px hairline rules, square
        // corners. Selection is white-on-black inversion; the spectral colours are state marks only.
        const ImVec4 PK_FIELD   = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
        const ImVec4 PK_WHITE   = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
        const ImVec4 PK_INK     = ImVec4(0.02f, 0.02f, 0.03f, 1.00f);   // text on a selected (white) cell
        const ImVec4 PK_READOUT = ImVec4(0.55f, 0.55f, 0.55f, 1.00f);   // 0x8c8c8c
        const ImVec4 PK_QUIET   = ImVec4(0.36f, 0.36f, 0.36f, 1.00f);   // 0x5c5c5c
        const ImVec4 PK_HAIR    = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);   // 0x262626
        const ImVec4 PK_FAINT   = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);   // 0x1a1a1a
        const ImVec4 PK_READY   = ImVec4(0.24f, 0.86f, 0.59f, 1.00f);   // 0x3ddc97
        const ImVec4 PK_CAUTION = ImVec4(1.00f, 0.77f, 0.00f, 1.00f);   // 0xffc400
        const ImVec4 PK_BLOCKED = ImVec4(1.00f, 0.23f, 0.19f, 1.00f);   // 0xff3b30
        const ImVec4 PK_INFO    = ImVec4(0.30f, 0.64f, 1.00f, 1.00f);   // 0x4da3ff

        // Push the prism look for everything drawn inside the panel field. 12 colors + 4 vars;
        // prism_style_pop() must mirror exactly.
        void prism_style_push()
        {
            ImGui::PushStyleColor(ImGuiCol_Text, PK_WHITE);
            ImGui::PushStyleColor(ImGuiCol_TextDisabled, PK_QUIET);
            ImGui::PushStyleColor(ImGuiCol_Separator, PK_HAIR);
            ImGui::PushStyleColor(ImGuiCol_Border, PK_HAIR);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, PK_FIELD);
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, PK_FAINT);
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, PK_HAIR);
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, PK_WHITE);
            ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, PK_WHITE);
            ImGui::PushStyleColor(ImGuiCol_Header, PK_HAIR);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.22f, 0.22f, 0.22f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, PK_HAIR);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_SeparatorTextBorderSize, 1.0f);
        }
        void prism_style_pop()
        {
            ImGui::PopStyleVar(4);
            ImGui::PopStyleColor(12);
        }

        // A segmented control: n items in one box. Selected item is light with dark text, the
        // prism panel's signature look; unselected is dark with grey text.
        bool segments(const char *id, const char *const *items, int n, int &cur, float total_w)
        {
            bool changed = false;
            ImGui::PushID(id);
            const float w = total_w / (float)n;
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
            for (int i = 0; i < n; ++i)
            {
                if (i > 0) ImGui::SameLine();
                const bool on = (cur == i);
                ImGui::PushStyleColor(ImGuiCol_Button, on ? PK_WHITE : PK_FIELD);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? PK_WHITE : PK_FAINT);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, on ? PK_WHITE : PK_FAINT);
                ImGui::PushStyleColor(ImGuiCol_Text, on ? PK_INK : PK_QUIET);
                if (ImGui::Button(items[i], ImVec2(w, 0.0f)) && !on)
                {
                    cur = i;
                    changed = true;
                }
                ImGui::PopStyleColor(4);
            }
            ImGui::PopStyleVar(2);
            ImGui::PopID();
            return changed;
        }

        // The label half of a row: text in the left column, everything after this call draws the
        // control at the column x. 140 fits four CJK glyphs at the panel's font scale.
        void row_label(const char *z, const char *e)
        {
            ImGui::TextUnformatted(T_(z, e));
            ImGui::SameLine(140.0f);
        }

        // ---- the custom grade table (Look 5), straight from prism's GradeSpec table ----
        // One slider per control, sections collapsed like the prism panel's pages. Values are in
        // the table's own units; every change repacks the b1 constants for the next dispatch and
        // the file is written when the slider is released.
        void grade_row(int i, float w)
        {
            const prism::GradeSpec &s = prism::GradeInfo(i);
            const std::string lbl = w2u(s.label);
            ImGui::TextUnformatted(lbl.c_str());
            ImGui::SameLine(140.0f);
            float v = mgpu::pfinish::grade_value(i);
            ImGui::SetNextItemWidth(w - 60.0f);
            if (ImGui::SliderFloat(w2u(s.key).c_str(), &v, s.min, s.max, ""))
            {
                mgpu::pfinish::set_grade_value(i, v);
                // R375: the grade table only reaches the picture at Look 5 (自定义).
                // Sliding any grade while a preset look is selected silently did
                // nothing - the exact "自定义调色盘失效" report. Switch for them.
                if (mgpu::pfinish::look() != 5u)
                {
                    mgpu::pfinish::set_look(5u);
                    (void)mgpu::gpu1::ui_ini_write("FinishLook", 5);
                }
            }
            ImGui::SameLine();
            // show the value in the table's own units, like the prism panel does
            switch (s.unit)
            {
            case prism::kUSigned: ImGui::Text("%+d", (int)(v + (v >= 0.0f ? 0.5f : -0.5f))); break;
            case prism::kUStops:  ImGui::Text("%+.2f", (double)v); break;
            default:              ImGui::Text("%.0f", (double)v); break;
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
                mgpu::pfinish::save_grade();
        }

        // One collapsible grade section: [first, last] controls under a header with a reset button.
        // open_default: 影调/色彩 start expanded like the prism panel's first pages.
        void grade_section(int first, int last, const char *z, const char *e, bool open_default)
        {
            ImGui::PushID(z);
            if (open_default) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            if (ImGui::CollapsingHeader(T_(z, e)))
            {
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 40.0f);
                if (ImGui::SmallButton(T_("重置", "reset")))
                {
                    for (int i = first; i <= last; ++i)
                        mgpu::pfinish::set_grade_value(i, prism::GradeInfo(i).def);
                    mgpu::pfinish::save_grade();
                }
                ImGui::Indent(8.0f);
                const float w = ImGui::GetContentRegionAvail().x;
                for (int i = first; i <= last; ++i)
                    grade_row(i, w);
                ImGui::Unindent(8.0f);
            }
            ImGui::PopID();
        }

        // ---- the frame-time bar chart (the prism panel's header) ----
        const int  kChartN = 140;
        float      g_chart_ms[kChartN] = {};
        long long  g_chart_n = 0;

        void frame_chart(const mgpu::gpu1::ui_state &st)
        {
            if (st.fps_consumed > 1.0)
            {
                g_chart_ms[g_chart_n % kChartN] = (float)(1000.0 / st.fps_consumed);
                ++g_chart_n;
            }
            const float avail = ImGui::GetContentRegionAvail().x;
            ImGui::TextUnformatted(T_("最近 140 帧", "last 140 frames"));
            float hi = 33.4f;
            for (int i = 0; i < kChartN && i < (int)g_chart_n; ++i)
                if (g_chart_ms[i] > hi) hi = g_chart_ms[i];
            char mb[32];
            snprintf(mb, sizeof mb, "%.0f ms", (double)hi);
            ImGui::SameLine(avail - 60.0f);
            ImGui::TextDisabled("%s", mb);

            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const float  cw = avail, ch = 44.0f;
            ImDrawList *dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p0, ImVec2(p0.x + cw, p0.y + ch), IM_COL32(0, 0, 0, 255));
            const int drawn_n = (int)(g_chart_n < kChartN ? g_chart_n : kChartN);
            const float bw = cw / (float)kChartN;
            for (int k = 0; k < drawn_n; ++k)
            {
                const int idx = (int)((g_chart_n - 1 - k) % kChartN);   // newest at the right edge
                const float m = g_chart_ms[idx];
                const float h = ch * (m / hi);
                const float x1 = p0.x + cw - (float)(k + 1) * bw;
                // white bars; the spectral colours appear only as state marks on slow frames
                // (>18 ms caution, >30 ms blocked), the prism barcode's convention.
                ImU32 col = IM_COL32(255, 255, 255, 255);
                if (m > 30.0f)      col = IM_COL32(255, 59, 48, 255);
                else if (m > 18.0f) col = IM_COL32(255, 196, 0, 255);
                dl->AddRectFilled(ImVec2(x1 + 0.5f, p0.y + ch - h),
                                  ImVec2(x1 + bw - 0.5f, p0.y + ch), col);
            }
            ImGui::Dummy(ImVec2(cw, ch));
        }
    }

    void draw_simple_panel()
    {
        load_if_changed(false);

        // R330: the full panel's install-layout warning, same words. A player in simple mode with
        // nvngx_dlssnr.dll beside the exe is heading for the crash and must see it here too.
        {
            const int layout = mgpu::gpu1::ui_install_layout();
            if (layout == 1)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.30f, 1.0f),
                                   "INSTALL PROBLEM - the neural stage will crash this game.");
                ImGui::TextUnformatted("nvngx_dlssnr.dll is next to the game's exe. The game loads it");
                ImGui::TextUnformatted("there and claims the wrong GPU before this add-on starts.");
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                                   "MOVE it into the mgpu folder next to the add-on, then restart.");
                ImGui::Separator();
                return;
            }
            else if (layout == 2)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                                   "nvngx_dlssnr.dll not found - neural rendering cannot start.");
                ImGui::TextUnformatted("Put it in the mgpu folder next to the add-on, then restart.");
                ImGui::Separator();
                return;
            }
        }

        // ---- the prism-style menu (fixed layout; panel_simple.txt no longer drives the draw) ----
        mgpu::gpu1::ui_state st;
        mgpu::gpu1::ui_read(st);

        // The prism field: a pure black child window with a 1px hairline border, the panel
        // sitting on it the way prism's overlay sits on the game. Style push first so the
        // child background/border pick it up.
        prism_style_push();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, PK_FIELD);
        ImGui::BeginChild("pf_field", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
        const float ctrl_w = ImGui::GetContentRegionAvail().x - 40.0f;

        // Title + armed status, the "Prism Core / 运行中" line. The state word carries the
        // spectral colour (green running, amber waiting), same as the prism panel's corner.
        ImGui::TextUnformatted("MGPU Prism 2.4");
        {
            const char *zs = st.armed ? T_("运行中", "running") : T_("等待画面", "waiting");
            const ImVec4 c = st.armed ? PK_READY : PK_CAUTION;
            const float w = ImGui::CalcTextSize(zs).x;
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - w);
            ImGui::TextColored(c, "| %s", zs);
        }
        ImGui::Spacing();

        frame_chart(st);

        // The primary readout is the compositor delivery cadence. Neural evaluation
        // counts remain internal diagnostics; game production is shown separately.
        {
            char b0[64], b1[64], b2[64];
            snprintf(b0, sizeof b0, "%.1f", st.present_fps);
            snprintf(b1, sizeof b1, "%.1f ms", st.fps_consumed > 0.01 ? 1000.0 / st.fps_consumed : 0.0);
            snprintf(b2, sizeof b2, "%.0f", (st.present_p99_ms > 0.01) ? 1000.0 / st.present_p99_ms : 0.0);
            ImGui::TextDisabled("%s", T_("画面 FPS   传输帧时间  1%% 低帧    MGPU GPU",
                                         "present fps   transport ms   1% low   MGPU GPU"));
            ImGui::TextUnformatted(b0);
            ImGui::SameLine(140.0f); ImGui::TextUnformatted(b1);
            ImGui::SameLine(280.0f); ImGui::TextUnformatted(b2);
            ImGui::SameLine(420.0f);
            ImGui::TextUnformatted(st.nr_ok ? "ON" : "-");
            ImGui::TextDisabled("%s", T_("游戏生产 fps", "game production fps"));
            ImGui::Text("%.1f", st.fps_produced);
        }
        ImGui::Separator();
        ImGui::Spacing();

        // 神经渲染: 关 / 开
        {
            row_label("神经渲染", "neural rendering");
            static const char *it[2] = {};
            if (it[0] == nullptr) { it[0] = T_("关", "OFF"); it[1] = T_("开", "ON"); }
            int cur = st.neural ? 1 : 0;
            if (segments("pf.on", it, 2, cur, ctrl_w * 0.5f))
                mgpu::gpu1::ui_set_neural(cur == 1);
        }

        // 色彩: the prism look presets (needs the finish module armed). Segment 6 = Look 5,
        // the full custom grade table (below).
        if (mgpu::pfinish::active())
        {
            row_label("色彩", "colour");
            static const char *lk[6] = {};
            if (lk[0] == nullptr)
            {
                lk[0] = T_("默认", "default"); lk[1] = T_("高还原", "faithful");
                lk[2] = T_("高饱和", "vivid"); lk[3] = T_("低饱和", "muted");
                lk[4] = T_("浓烈", "rich"); lk[5] = T_("自定义", "custom");
            }
            int cur = (int)mgpu::pfinish::look();
            if (segments("pf.look", lk, 6, cur, ctrl_w))
            {
                mgpu::pfinish::set_look((unsigned)cur);
                (void)mgpu::gpu1::ui_ini_write("FinishLook", cur);
            }

            // Look 5: the custom grade table, six collapsible sections in the prism panel's order.
            if (mgpu::pfinish::look() == 5u)
            {
                grade_section(prism::kGExposure,  prism::kGMidtones,  "影调", "tone",     true);
                grade_section(prism::kGTemp,      prism::kGMix,       "色彩", "colour",   true);
                grade_section(prism::kGShadowHue, prism::kGHiAmt,     "色轮", "wheels",   false);
                grade_section(prism::kGHueShift,  prism::kGHueLight + 5, "色相", "hues",  false);
                grade_section(prism::kGCurve,     prism::kGCurve + 19,"曲线", "curves",   false);
                grade_section(prism::kGDensity,   prism::kGGrain,     "胶片", "film",     false);
            }
        }
        else
        {
            row_label("色彩", "colour");
            ImGui::TextDisabled("%s", T_("ini 无 Finish=1（重启生效）", "Finish=1 not in mgpu.ini (restart)"));
        }

        // 强度: PSYNC and legacy NGX have independent multipliers; never drive both at once.
        {
            row_label("强度", "intensity");
            const bool sync = mgpu::psync::active();
            float v = sync ? mgpu::psync::intensity() : st.intensity[0];
            ImGui::SetNextItemWidth(ctrl_w - 52.0f);
            if (ImGui::SliderFloat("##pf.intensity", &v, 0.0f, 3.0f, ""))
            {
                if (sync)
                {
                    mgpu::psync::set_intensity(v);
                    (void)mgpu::gpu1::ui_ini_write_float("PrismSyncIntensity", v);
                }
                else mgpu::gpu1::ui_set_intensity(0u, v);
            }
            ImGui::SameLine();
            ImGui::Text("%.0f%%", (double)(v * 100.0f));
        }

        // 遍数: 1 | 2 | 3   (MAX_PASSES is 4 at runtime; 3 is the owner's target)
        {
            row_label("遍数", "passes");
            static const char *ps[3] = { "1", "2", "3" };
            int cur = (int)st.passes - 1;
            if (cur < 0) cur = 0;
            if (cur > 2) cur = 2;
            if (segments("pf.passes", ps, 3, cur, ctrl_w * 0.5f))
                mgpu::gpu1::ui_set_passes((unsigned)(cur + 1));
        }

        // 0.5.1 per-pass rows (R356/R364): every RUNNING pass gets its own network
        // (Light-FP8 / Full-FP8) and resolution (Residual Neural / Native) pair.
        for (unsigned i = 0; i < st.passes && i < 3u; ++i)
        {
            char lbl[40];
            snprintf(lbl, sizeof lbl, "P%u 网络", i + 1u);
            row_label(lbl, "pass network");
            ps_net(i);
            snprintf(lbl, sizeof lbl, "P%u 分辨率", i + 1u);
            row_label(lbl, "pass resolution");
            ps_res(i);
        }

        // 节奏: run the network every 1, 2 or 4 frames, independently of Passes.
        {
            row_label("节奏", "cadence");
            static const char *cs[3] = {};
            if (cs[0] == nullptr)
            {
                cs[0] = T_("每 1 帧", "every 1"); cs[1] = T_("每 2 帧", "every 2"); cs[2] = T_("每 4 帧", "every 4");
            }
            int cur = st.nr_cadence == 4 ? 2 : st.nr_cadence == 2 ? 1 : 0;
            if (segments("pf.cadence", cs, 3, cur, ctrl_w))
            {
                const unsigned v = cur == 2 ? 4u : cur == 1 ? 2u : 1u;
                mgpu::gpu1::ui_set_cadence(v);
            }
        }

        // 网络分辨率: the NRS scale slider, 0.30..1.00, live (needs NRScale= in mgpu.ini).
        if (st.nrs_on)
        {
            row_label("网络分辨率", "net scale");
            float sc = st.nrs_scale;
            ImGui::SetNextItemWidth(ctrl_w - 52.0f);
            if (ImGui::SliderFloat("##pf.nrs", &sc, 0.30f, 1.0f, ""))
                mgpu::gpu1::ui_nrs_stage(sc);
            ImGui::SameLine();
            ImGui::Text("%.0f%%", (double)(sc * 100.0f));
        }
        else
        {
            row_label("网络分辨率", "net scale");
            ImGui::TextDisabled("%s", T_("ini 无 NRScale=（重启生效）", "NRScale= not in mgpu.ini (restart)"));
        }

        // 角色守护: PSYNC owns it on the synchronous path; otherwise PRISM-FINISH does.
        const bool sync_guard = mgpu::psync::active();
        if (sync_guard || mgpu::pfinish::active())
        {
            row_label("角色守护", "character guard");
            float v = sync_guard ? mgpu::psync::guard() : mgpu::pfinish::guard();
            ImGui::SetNextItemWidth(ctrl_w - 52.0f);
            if (ImGui::SliderFloat("##pf.guard", &v, 0.0f, 1.0f, ""))
            {
                if (sync_guard)
                {
                    mgpu::psync::set_guard(v);
                    (void)mgpu::gpu1::ui_ini_write_float("PrismSyncGuard", v);
                }
                else
                {
                    mgpu::pfinish::set_guard(v);
                    (void)mgpu::gpu1::ui_ini_write_float("FinishGuard", v);
                }
            }
            ImGui::SameLine();
            ImGui::Text("%.0f%%", (double)(v * 100.0f));
        }
        else
        {
            row_label("角色守护", "character guard");
            ImGui::TextDisabled("-");
        }

        // 二次元: protect pale / cel-shaded skin and vivid character colours only
        // where the neural result darkened them. Live on the next finish dispatch.
        {
            row_label("二次元", "anime colour protection");
            if (mgpu::pfinish::active())
            {
                static const char *an[2] = {};
                if (an[0] == nullptr) { an[0] = T_("关", "OFF"); an[1] = T_("开", "ON"); }
                int cur = mgpu::pfinish::anime() ? 1 : 0;
                if (segments("pf.anime", an, 2, cur, ctrl_w * 0.5f))
                {
                    mgpu::pfinish::set_anime(cur == 1);
                    (void)mgpu::gpu1::ui_ini_write("FinishAnime", cur);
                }
            }
            else ImGui::TextDisabled("-");
        }

        // 分屏对比: 关 / 开 (the P7.4 split view, input left of the seam, output right).
        {
            row_label("分屏对比", "split compare");
            static const char *it[2] = {};
            if (it[0] == nullptr) { it[0] = T_("关", "OFF"); it[1] = T_("开", "ON"); }
            int cur = (st.present_mode == 2) ? 1 : 0;
            if (segments("pf.split", it, 2, cur, ctrl_w * 0.5f))
                mgpu::gpu1::ui_set_present_mode(cur == 1 ? 2 : 0);
        }

        // 帧率角标: not ported yet (its own change - a badge on the game screen, which the
        // overlay panel cannot draw; it needs the present chain's font pass).
        {
            row_label("帧率角标", "fps badge");
            ImGui::BeginDisabled();
            static const char *it[2] = {};
            if (it[0] == nullptr) { it[0] = T_("关", "OFF"); it[1] = T_("开", "ON"); }
            int cur = 0;
            (void)segments("pf.hud", it, 2, cur, ctrl_w * 0.5f);
            ImGui::EndDisabled();
        }

        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::SmallButton(L("ui.reload", "reload", "reload").c_str()))
            reload();

        // footer, the prism panel's bottom hairline + quiet hint line
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("%s", T_("改动实时生效 · 滑条松手写入 ini · 色彩自定义档见下级分区",
                                     "changes apply live - sliders write ini on release"));
        ImGui::EndChild();
        ImGui::PopStyleColor();   // ChildBg
        prism_style_pop();
    }

    void draw_editor()
    {
        // R330 (F6): the checkbox is there for everyone; the rest only when panel_simple.txt is beside
        // the add-on. Without the file nothing is read and nothing is logged.
        ImGui::SeparatorText("Simple panel");
        bool on = simple_flag() != 0;   // R330
        if (ImGui::Checkbox("PanelSimple - show the simple panel instead of this one", &on))
            set_enabled(on);
        if (!file_present()) return;
        load_if_changed(false);
        register_rows();   // R330: the count below is right before the simple panel was ever drawn
        ImGui::SameLine();
        ImGui::TextDisabled("cjk font: %s | %d row(s) in panel_simple.txt, %d registered in this build",
                            cjk() ? "yes" : "NO (labels fall back to English)",
                            (int)g_layout.size(), (int)g_rows.size());
        if (ImGui::SmallButton("open panel_simple.txt")) open_editor();
        ImGui::SameLine();
        if (ImGui::SmallButton("export items")) export_items();
        ImGui::SameLine();
        if (ImGui::SmallButton(L("ui.reload", "reload", "reload").c_str())) reload();
        ImGui::TextDisabled("A row appears in the simple panel only if panel_simple.txt names it, and"
                            " it must have a control in this build to be drawn.");
    }
}
#else

    // No ImGui headers in this build: keep the non-visual API linkable.
    bool cjk() { return false; }
    bool enabled() { return false; }
    void set_enabled(bool on)
    {
        (void)mgpu::gpu1::ui_ini_write_word("PanelSimple", on ? "1" : "0");
        g_simple.store(on ? 1 : 0, std::memory_order_relaxed);
    }
    void toggle() { set_enabled(simple_flag() == 0); }
    void label(const char *, const char *) {}
    void help(const char *) {}
    void draw_simple_panel() {}
    void draw_editor() {}
}

#endif  // MGPU_HAVE_IMGUI
