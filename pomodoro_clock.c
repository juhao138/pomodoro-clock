/*
 * pomodoro_clock.c —— 桌面时钟 + 番茄钟 + 待办清单（Windows，Win32 + GDI+）
 *
 * 设计原则：时钟是主角。待办清单只在下方小块区域定时轮播（默认每 30 秒露 8 秒），
 *           也可以按 L 手动叫出来；真正的编辑放在独立的窗口里（按 E 打开）。
 *
 * 构建：
 *   zig cc -O2 -std=c11 -o 桌面时钟番茄钟.exe pomodoro_clock.c `
 *          _build/clock.res -lgdiplus -lgdi32 -luser32 -lole32 -lcomctl32
 */

#define UNICODE 1
#define _UNICODE 1

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <imm.h>
#include <gdiplus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ------------------------------------------------------------------ 配色 */

#define C_BG_TOP     0x0B0F1A
#define C_BG_BOT     0x151D2E
#define C_TEXT       0xF4F7FC
#define C_MUTED      0x9AA5B8
#define C_DIM        0x6C778C
#define C_TRACK      0x232B3D
#define C_CARD       0x141B2A
#define C_CARD_LINE  0x28324A
#define C_WORK       0xFF6B5A
#define C_BREAK      0x35C48F
#define C_LONG       0x4EA8FF
#define C_SELECT     0x1E2942

static int argb(unsigned rgb, int a) { return (int)(((unsigned)a << 24) | rgb); }
static void palette_set(int dark);
static int g_dark;

/* ------------------------------------------------------------------ 番茄钟 */

typedef enum { PHASE_WORK = 0, PHASE_SHORT = 1, PHASE_LONG = 2 } Phase;

typedef struct {
    Phase phase;
    double remaining;
    double total;
    int running;
    int completed;
    int work_min, short_min, long_min;
    int long_every;
    int transitions;
} Pomodoro;

static const wchar_t *phase_name_w(Phase p) {
    switch (p) {
    case PHASE_WORK:  return L"专 注";
    case PHASE_SHORT: return L"短 休 息";
    default:          return L"长 休 息";
    }
}

static const wchar_t *phase_name_plain(Phase p) {
    switch (p) {
    case PHASE_WORK:  return L"专注";
    case PHASE_SHORT: return L"短休息";
    default:          return L"长休息";
    }
}

static unsigned phase_color(Phase p) {
    switch (p) {
    case PHASE_WORK:  return C_WORK;
    case PHASE_SHORT: return C_BREAK;
    default:          return C_LONG;
    }
}

static double phase_seconds(const Pomodoro *p, Phase ph) {
    switch (ph) {
    case PHASE_WORK:  return p->work_min * 60.0;
    case PHASE_SHORT: return p->short_min * 60.0;
    default:          return p->long_min * 60.0;
    }
}

static void pomo_init(Pomodoro *p) {
    memset(p, 0, sizeof *p);
    p->work_min = 25;
    p->short_min = 5;
    p->long_min = 15;
    p->long_every = 4;
    p->phase = PHASE_WORK;
    p->total = p->remaining = p->work_min * 60.0;
}

static void pomo_reset(Pomodoro *p) {
    p->phase = PHASE_WORK;
    p->total = p->remaining = p->work_min * 60.0;
    p->running = 0;
}

static void pomo_advance(Pomodoro *p) {
    Phase next;
    if (p->phase == PHASE_WORK) {
        p->completed++;
        next = (p->completed % p->long_every == 0) ? PHASE_LONG : PHASE_SHORT;
    } else {
        next = PHASE_WORK;
    }
    p->phase = next;
    p->total = p->remaining = phase_seconds(p, next);
    p->transitions++;
}

/* 超出的时间结转到下一阶段，不丢失 */
static int pomo_tick(Pomodoro *p, double dt) {
    if (!p->running || dt <= 0) return 0;
    int changed = 0;
    p->remaining -= dt;
    for (int guard = 0; p->remaining <= 0 && guard < 10000; guard++) {
        double over = -p->remaining;
        p->remaining = 0;
        pomo_advance(p);
        changed = 1;
        if (p->total <= 0) { p->remaining = 0; break; }
        p->remaining -= over;
    }
    return changed;
}

static void pomo_skip(Pomodoro *p) { pomo_advance(p); }

/* ------------------------------------------------------------------ 待办清单 */

#define MAX_TODOS   128
#define TODO_TEXT   160

typedef struct {
    wchar_t text[TODO_TEXT];
    int done;
} Todo;

static Todo g_todos[MAX_TODOS];
static int g_ntodos = 0;
static int g_todo_sel = 0;
static wchar_t g_todo_path[MAX_PATH];

/* 目录能否写：在里面建个临时文件试试 */
static int dir_writable(const wchar_t *dir) {
    wchar_t probe[MAX_PATH];
    swprintf(probe, MAX_PATH, L"%s\\~mp3rn_probe.tmp", dir);
    HANDLE f = CreateFileW(probe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    CloseHandle(f);
    DeleteFileW(probe);
    return 1;
}

/* 清单文件位置：优先 exe 同目录（便携），不可写才退到 %APPDATA% */
static void todo_path_init(void) {
    wchar_t exedir[MAX_PATH] = { 0 };
    GetModuleFileNameW(NULL, exedir, MAX_PATH);
    wchar_t *slash = wcsrchr(exedir, L'\\');
    if (slash) *slash = 0;

    if (exedir[0] && dir_writable(exedir)) {
        swprintf(g_todo_path, MAX_PATH, L"%s\\todo.txt", exedir);
        return;
    }

    wchar_t appdata[MAX_PATH] = { 0 };
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        wchar_t sub[MAX_PATH];
        swprintf(sub, MAX_PATH, L"%s\\PomodoroClock", appdata);
        CreateDirectoryW(sub, NULL);
        if (dir_writable(sub)) {
            swprintf(g_todo_path, MAX_PATH, L"%s\\todo.txt", sub);
            return;
        }
    }
    /* 都不行：仍然指向 exe 目录，写失败时编辑器里会提示 */
    if (exedir[0]) swprintf(g_todo_path, MAX_PATH, L"%s\\todo.txt", exedir);
}

static int todo_save(void) {
    if (!g_todo_path[0]) return -1;
    HANDLE f = CreateFileW(g_todo_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return -(int)GetLastError();

    char buf[8192];
    int len = 0;
    for (int i = 0; i < g_ntodos; i++) {
        char utf8[TODO_TEXT * 4];
        int n = WideCharToMultiByte(CP_UTF8, 0, g_todos[i].text, -1, utf8,
                                    sizeof utf8, NULL, NULL);
        if (n <= 1) continue;
        int w = snprintf(buf + len, sizeof buf - (size_t)len, "%d\t%s\n",
                         g_todos[i].done ? 1 : 0, utf8);
        if (w < 0 || len + w >= (int)sizeof buf) break;
        len += w;
    }
    DWORD wrote = 0;
    if (len > 0) WriteFile(f, buf, (DWORD)len, &wrote, NULL);
    CloseHandle(f);
    return wrote == (DWORD)len ? 0 : -(int)GetLastError();
}

static void todo_load(void) {
    g_ntodos = 0;
    if (!g_todo_path[0]) return;
    HANDLE f = CreateFileW(g_todo_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;

    char buf[8192];
    DWORD got = 0;
    ReadFile(f, buf, sizeof buf - 1, &got, NULL);
    CloseHandle(f);
    buf[got] = 0;

    char *line = buf;
    while (line && *line && g_ntodos < MAX_TODOS) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        int done = (*line == '1');
        char *tab = strchr(line, '\t');
        if (tab) {
            int n = MultiByteToWideChar(CP_UTF8, 0, tab + 1, -1,
                                        g_todos[g_ntodos].text, TODO_TEXT);
            if (n > 1) {
                g_todos[g_ntodos].done = done;
                g_ntodos++;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
}

static int todo_add(const wchar_t *s) {
    if (!s || !s[0] || g_ntodos >= MAX_TODOS) return -1;
    wcsncpy(g_todos[g_ntodos].text, s, TODO_TEXT - 1);
    g_todos[g_ntodos].text[TODO_TEXT - 1] = 0;
    g_todos[g_ntodos].done = 0;
    g_ntodos++;
    return g_ntodos - 1;
}

static void todo_delete(int idx) {
    if (idx < 0 || idx >= g_ntodos) return;
    for (int i = idx; i < g_ntodos - 1; i++) g_todos[i] = g_todos[i + 1];
    g_ntodos--;
    if (g_todo_sel >= g_ntodos) g_todo_sel = g_ntodos - 1;
    if (g_todo_sel < 0) g_todo_sel = 0;
}

static void todo_toggle(int idx) {
    if (idx < 0 || idx >= g_ntodos) return;
    g_todos[idx].done = !g_todos[idx].done;
}

static int todo_edit(int idx, const wchar_t *s) {
    if (idx < 0 || idx >= g_ntodos || !s || !s[0]) return -1;
    wcsncpy(g_todos[idx].text, s, TODO_TEXT - 1);
    g_todos[idx].text[TODO_TEXT - 1] = 0;
    return idx;
}

static int todo_done_count(void) {
    int c = 0;
    for (int i = 0; i < g_ntodos; i++) if (g_todos[i].done) c++;
    return c;
}

/* ------------------------------------------------------------------ 全局 */

static HWND g_hwnd;
static HWND g_editor;
static HWND g_ed_edit, g_ed_list, g_ed_add, g_ed_toggle, g_ed_delete, g_ed_close;
static HWND g_ed_title, g_ed_hint;
static HWND g_bg_section, g_bg_path_lbl, g_bg_pick, g_bg_clear;
static HFONT g_ed_font, g_ed_font_title;
static HBRUSH g_ed_bg_brush, g_ed_edit_brush;
static RECT g_ed_field, g_ed_list_rect;
static int g_ed_index = -1;
static int g_ed_registered = 0;

static Pomodoro g_pomo;
static ULONGLONG g_last_tick;
static int g_opacity_index = 0;
static const BYTE kOpacity[] = { 255, 232, 200, 168 };
static int g_mute = 0;
static int g_fullscreen = 1;

/* 待办轮播 */
static int g_list_every = 30;
static int g_list_dur = 8;
static double g_list_clock = 0;
static double g_list_alpha = 0;
static int g_list_pinned = 0;

/* 背景动态明暗：一个很慢的正弦，周期默认 26 秒 */
static double g_breath_phase = 0.0;
static double g_breath_period = 18.0;

static wchar_t g_title[256];

/* GDI+ */
static GpGraphics *g_gp;
static HDC g_memdc;
static HBITMAP g_membmp;
static void *g_bits;
static int g_buf_w, g_buf_h;

typedef struct { int size; int kind; GpFont *font; } FontSlot;
#define FONT_CACHE 64
static FontSlot g_fonts[FONT_CACHE];
static int g_nfonts = 0;

#define FONT_DISPLAY 0
#define FONT_UI      1
#define FONT_UI_BOLD 2
#define FONT_LATIN   3

/* ------------------------------------------------------------------ 提示音 */

/* 程序内合成的柔和风铃：三个音的上行琶音 + 泛音 + 指数衰减，
   写进内存里的 WAV，用 PlaySound 异步播放，不依赖任何外部音频文件。 */
static void *g_chime = NULL;
static int   g_chime_len = 0;

static void put_u16(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void put_u32(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static void chime_build(void) {
    if (g_chime) return;

    const int rate = 22050;
    const double dur = 1.75;
    int samples = (int)(rate * dur);
    int data_bytes = samples * 2;
    unsigned char *pcm = malloc((size_t)(44 + data_bytes));
    if (!pcm) return;

    memcpy(pcm, "RIFF", 4);
    put_u32(pcm + 4, (unsigned)(36 + data_bytes));
    memcpy(pcm + 8, "WAVEfmt ", 8);
    put_u32(pcm + 16, 16);
    put_u16(pcm + 20, 1);              /* PCM */
    put_u16(pcm + 22, 1);              /* 单声道 */
    put_u32(pcm + 24, rate);
    put_u32(pcm + 28, (unsigned)(rate * 2));
    put_u16(pcm + 32, 2);
    put_u16(pcm + 34, 16);
    memcpy(pcm + 36, "data", 4);
    put_u32(pcm + 40, (unsigned)data_bytes);

    /* E5 - G5 - C6，柔和的大三度琶音 */
    const double freq[3] = { 659.26, 783.99, 1046.50 };
    const double start[3] = { 0.00, 0.16, 0.32 };
    const double gain[3]  = { 0.55, 0.42, 0.34 };
    short *out = (short *)(pcm + 44);

    for (int i = 0; i < samples; i++) {
        double t = (double)i / (double)rate;
        double v = 0.0;
        for (int n = 0; n < 3; n++) {
            double lt = t - start[n];
            if (lt < 0) continue;
            double env = exp(-3.1 * lt);
            if (lt < 0.014) env *= lt / 0.014;      /* 14ms 起音，避免爆音 */
            /* 基频 + 二次/三次泛音，听起来像风铃 */
            double w = sin(6.283185307179586 * freq[n] * lt)
                     + 0.30 * sin(6.283185307179586 * 2.0 * freq[n] * lt)
                     + 0.10 * sin(6.283185307179586 * 3.0 * freq[n] * lt);
            v += gain[n] * env * w;
        }
        v *= 0.34;                                   /* 总音量：听得到但不刺耳 */
        if (v > 1.0) v = 1.0;
        if (v < -1.0) v = -1.0;
        out[i] = (short)(v * 32000.0);
    }

    g_chime = pcm;
    g_chime_len = 44 + data_bytes;
}

static void chime_play(void) {
    if (g_mute) return;
    chime_build();
    if (!g_chime) return;
    PlaySoundW((LPCWSTR)g_chime, NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

/* ------------------------------------------------------------------ GDI+ 工具 */

static GpSolidFill *brush_new(int argb_color) {
    GpSolidFill *b = NULL;
    GdipCreateSolidFill((ARGB)argb_color, &b);
    return b;
}

static GpPath *round_rect_path(float x, float y, float w, float h, float r) {
    if (r < 0.5f) r = 0.5f;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    GpPath *p = NULL;
    GdipCreatePath(FillModeAlternate, &p);
    if (!p) return NULL;
    GdipAddPathArc(p, x, y, r * 2, r * 2, 180, 90);
    GdipAddPathArc(p, x + w - r * 2, y, r * 2, r * 2, 270, 90);
    GdipAddPathArc(p, x + w - r * 2, y + h - r * 2, r * 2, r * 2, 0, 90);
    GdipAddPathArc(p, x, y + h - r * 2, r * 2, r * 2, 90, 90);
    GdipClosePathFigure(p);
    return p;
}

static void fill_round(GpGraphics *g, int argb_color, float x, float y,
                       float w, float h, float r) {
    GpPath *p = round_rect_path(x, y, w, h, r);
    if (!p) return;
    GpSolidFill *b = brush_new(argb_color);
    if (b) {
        GdipFillPath(g, (GpBrush *)b, p);
        GdipDeleteBrush((GpBrush *)b);
    }
    GdipDeletePath(p);
}

static void stroke_round(GpGraphics *g, int argb_color, float x, float y,
                         float w, float h, float r, float width) {
    GpPath *p = round_rect_path(x, y, w, h, r);
    if (!p) return;
    GpPen *pen = NULL;
    GdipCreatePen1((ARGB)argb_color, width, UnitPixel, &pen);
    if (pen) {
        GdipDrawPath(g, pen, p);
        GdipDeletePen(pen);
    }
    GdipDeletePath(p);
}

static GpFontFamily *g_family_ui = NULL;
static GpFontFamily *g_family_display = NULL;
static GpFontFamily *g_family_latin = NULL;
static int g_display_needs_bold = 0;
static wchar_t g_face_ui[LF_FACESIZE] = L"Microsoft YaHei UI";
static wchar_t g_face_display[LF_FACESIZE] = L"Segoe UI Semibold";
static wchar_t g_face_latin_used[LF_FACESIZE] = L"";
static wchar_t g_face_ui_used[LF_FACESIZE] = L"";
static wchar_t g_face_display_used[LF_FACESIZE] = L"";

/* 按候选名依次尝试，返回第一个能加载的族 */
static GpFontFamily *try_family(const wchar_t *const *names, int n, wchar_t *used) {
    for (int i = 0; i < n; i++) {
        GpFontFamily *fam = NULL;
        if (GdipCreateFontFamilyFromName(names[i], NULL, &fam) == Ok && fam) {
            if (used) wcsncpy(used, names[i], LF_FACESIZE - 1);
            return fam;
        }
    }
    return NULL;
}

/* 中文环境下苹方的族名是「苹方 常规」这种带字重的，英文环境是 PingFang SC，
   所以两套名字都要试。 */
static void resolve_families(void) {
    /* Windows 上一定有微软雅黑；苹方只有少数机器装了，放最后当彩蛋 */
    static const wchar_t *ui_names[] = {
        L"Microsoft YaHei UI", L"微软雅黑",
        L"PingFang SC", L"苹方 常规",
        L"Segoe UI"
    };
    static const wchar_t *display_names[] = {
        L"Microsoft YaHei UI", L"微软雅黑",
        L"PingFang SC Medium", L"PingFang SC",
        L"Segoe UI Semibold", L"Segoe UI"
    };

    static const wchar_t *latin_names[] = {
        L"Bahnschrift", L"Bahnschrift SemiBold", L"Segoe UI Semibold", L"Segoe UI"
    };
    g_family_latin = try_family(latin_names,
                                (int)(sizeof latin_names / sizeof latin_names[0]),
                                g_face_latin_used);
    g_family_ui = try_family(ui_names, (int)(sizeof ui_names / sizeof ui_names[0]), g_face_ui_used);
    g_family_display = try_family(display_names,
                                  (int)(sizeof display_names / sizeof display_names[0]),
                                  g_face_display_used);
    if (!g_family_ui) g_family_ui = g_family_display;
    if (!g_family_display) g_family_display = g_family_ui;

    /* 只有在没拿到真正的中等/半粗字重时，才用 Bold 补 */
    g_display_needs_bold = 0;   /* 雅黑只有常规/粗体，先用常规看气质 */
    if (g_face_display_used[0]) {
        int has_weight = (wcsstr(g_face_display_used, L"Medium") != NULL ||
                          wcsstr(g_face_display_used, L"Semibold") != NULL ||
                          wcsstr(g_face_display_used, L"中等") != NULL ||
                          wcsstr(g_face_display_used, L"粗体") != NULL ||
                          wcsstr(g_face_display_used, L"粗") != NULL);
        if (!has_weight) g_display_needs_bold = 1;
    }
    if (g_face_ui_used[0]) {
        wcsncpy(g_face_ui, g_face_ui_used, LF_FACESIZE - 1);
        g_face_ui[LF_FACESIZE - 1] = 0;
    }
    if (g_face_display_used[0]) {
        wcsncpy(g_face_display, g_face_display_used, LF_FACESIZE - 1);
        g_face_display[LF_FACESIZE - 1] = 0;
    }
}

static GpFont *font_get(int size_px, int kind) {
    for (int i = 0; i < g_nfonts; i++)
        if (g_fonts[i].size == size_px && g_fonts[i].kind == kind)
            return g_fonts[i].font;

    GpFontFamily *family = g_family_ui;
    INT style = FontStyleRegular;
    switch (kind) {
    case FONT_DISPLAY:
        family = g_family_latin ? g_family_latin : g_family_display;
        style = FontStyleBold;
        break;
    case FONT_UI_BOLD:
        family = g_family_ui;
        style = FontStyleBold;
        break;
    case FONT_LATIN:
        family = g_family_latin ? g_family_latin : g_family_ui;
        break;
    default:
        family = g_family_ui;
        break;
    }
    if (!family) family = g_family_ui;

    GpFont *font = NULL;
    if (kind == FONT_DISPLAY && g_face_latin_used[0]) {
        LOGFONTW lf;
        memset(&lf, 0, sizeof lf);
        lf.lfHeight = -size_px;
        lf.lfWeight = FW_BOLD;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = ANTIALIASED_QUALITY;
        wcsncpy(lf.lfFaceName, g_face_latin_used, LF_FACESIZE - 1);
        GdipCreateFontFromLogfontW(NULL, &lf, &font);
        if (font) {
            /* GDI+ 拿不到字重时再退回构造字体 */
        }
    }
    if (!font) GdipCreateFont(family, (REAL)size_px, style, UnitPixel, &font);
    if (font && g_nfonts < FONT_CACHE) {
        g_fonts[g_nfonts].size = size_px;
        g_fonts[g_nfonts].kind = kind;
        g_fonts[g_nfonts].font = font;
        g_nfonts++;
    }
    return font;
}

static void text_draw_a(GpGraphics *g, const wchar_t *s, GpFont *font,
                        unsigned rgb, int alpha, float x, float y, float w, float h,
                        int align) {
    if (!font || !s || !s[0] || alpha <= 0) return;

    /* 小字用 ClearType（贴像素更锐利），大字用灰度抗锯齿（没有彩边、更圆润） */
    int size = 0;
    for (int i = 0; i < g_nfonts; i++)
        if (g_fonts[i].font == font) { size = g_fonts[i].size; break; }
    int small = (size < 40);
    GdipSetTextRenderingHint(g, small ? TextRenderingHintClearTypeGridFit
                                      : TextRenderingHintAntiAlias);

    /* 小字必须落在整像素上，否则 ClearType 会糊成一团 */
    if (small) {
        x = floorf(x + 0.5f);
        y = floorf(y + 0.5f);
        w = floorf(w + 0.5f);
        h = floorf(h + 0.5f);
    }
    GpStringFormat *fmt = NULL;
    GdipCreateStringFormat(0, LANG_NEUTRAL, &fmt);
    GdipSetStringFormatAlign(fmt, (StringAlignment)align);
    GdipSetStringFormatLineAlign(fmt, StringAlignmentCenter);
    GdipSetStringFormatTrimming(fmt, StringTrimmingEllipsisCharacter);
    GpSolidFill *b = brush_new(argb(rgb, alpha));
    RectF rc = { x, y, w, h };
    if (b) {
        GdipDrawString(g, s, -1, font, &rc, fmt, (GpBrush *)b);
        GdipDeleteBrush((GpBrush *)b);
    }
    GdipDeleteStringFormat(fmt);
    GdipSetTextRenderingHint(g, TextRenderingHintAntiAlias);
}

static void arc_draw(GpGraphics *g, float cx, float cy, float radius, float thick,
                     float start, float sweep, int argb_color, int round) {
    GpPen *pen = NULL;
    GdipCreatePen1((ARGB)argb_color, thick, UnitPixel, &pen);
    if (!pen) return;
    if (round) {
        GdipSetPenStartCap(pen, LineCapRound);
        GdipSetPenEndCap(pen, LineCapRound);
    }
    float d = radius * 2;
    GdipDrawArc(g, pen, cx - radius, cy - radius, d, d, start, sweep);
    GdipDeletePen(pen);
}

static void dot_draw(GpGraphics *g, float cx, float cy, float r, int argb_color) {
    GpSolidFill *b = brush_new(argb_color);
    if (!b) return;
    GdipFillEllipse(g, (GpBrush *)b, cx - r, cy - r, r * 2, r * 2);
    GdipDeleteBrush((GpBrush *)b);
}

/* 柔和光斑：叠层椭圆。PathGradient 的 sigma 混合在部分环境下不生效，
   这里用多次低 alpha 叠加，向外二次衰减，没有硬边。 */
static void glow(GpGraphics *g, float cx, float cy, float r,
                 unsigned rgb, int a_center) {
    const int steps = 56;
    for (int i = steps; i >= 1; i--) {
        float t = (float)i / (float)steps;      /* 1 = 最外圈 */
        float rr = r * t;
        int a = (int)(a_center * (1.0f - t) * (1.0f - t));
        if (a <= 0) continue;
        GpSolidFill *b = brush_new(argb(rgb, a));
        if (!b) continue;
        GdipFillEllipse(g, (GpBrush *)b, cx - rr, cy - rr, rr * 2, rr * 2);
        GdipDeleteBrush((GpBrush *)b);
    }
}

/* 背景缓存两张：暗相（光晕弱）+ 亮相（光晕强），逐帧 AlphaBlend 交叉淡入，
   这样呼吸的是极光本身，不是盖一层灰。 */
static HDC g_bgdc[2];
static HBITMAP g_bgbmp[2];
static int g_bg_w, g_bg_h;
static int g_bgc_alpha;      /* 合成缓存的当前 alpha 档，-1 = 失效 */

/* ---------------- 自定义背景图片 ---------------- */

static wchar_t g_bg_image[MAX_PATH] = { 0 };   /* 空 = 不用图片 */
static GpImage *g_bg_img = NULL;
static wchar_t g_settings_path[MAX_PATH] = { 0 };

static void settings_path_init(void) {
    wchar_t exedir[MAX_PATH] = { 0 };
    GetModuleFileNameW(NULL, exedir, MAX_PATH);
    wchar_t *slash = wcsrchr(exedir, L'\\');
    if (slash) *slash = 0;
    swprintf(g_settings_path, MAX_PATH, L"%s\\settings.ini", exedir);
}

static void settings_load(void) {
    if (!g_settings_path[0]) return;
    FILE *f = _wfopen(g_settings_path, L"r, ccs=UTF-8");
    if (!f) return;
    wchar_t line[1024];
    while (fgetws(line, 1024, f)) {
        size_t n = wcslen(line);
        while (n && (line[n-1] == L'\n' || line[n-1] == L'\r')) line[--n] = 0;
        if (!wcsncmp(line, L"bg_image=", 9)) {
            wcsncpy(g_bg_image, line + 9, MAX_PATH - 1);
            g_bg_image[MAX_PATH - 1] = 0;
        }
        if (!wcsncmp(line, L"theme=", 6)) {
            palette_set(!wcscmp(line + 6, L"dark"));
        }
    }
    fclose(f);
}

static void settings_save(void) {
    if (!g_settings_path[0]) return;
    FILE *f = _wfopen(g_settings_path, L"w, ccs=UTF-8");
    if (!f) return;
    fwprintf(f, L"bg_image=%ls\n", g_bg_image);
    fwprintf(f, L"theme=%ls\n", g_dark ? L"dark" : L"light");
    fclose(f);
}

static void bg_image_free(void) {
    if (g_bg_img) { GdipDisposeImage(g_bg_img); g_bg_img = NULL; }
    g_bg_w = 0;
    g_bg_h = 0;
    g_bgc_alpha = -1;
}

static int bg_image_load(const wchar_t *path) {
    bg_image_free();
    GpBitmap *src = NULL;
    if (GdipCreateBitmapFromFile(path, &src) != Ok || !src) return 0;

    /* 预缩放到屏幕尺寸：原图可能几千万像素，留着既占内存又拖慢重绘 */
    UINT iw = 0, ih = 0;
    GdipGetImageWidth((GpImage *)src, &iw);
    GdipGetImageHeight((GpImage *)src, &ih);
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    if (iw > (UINT)(sw * 1.2) || ih > (UINT)(sh * 1.2)) {
        double sc = (double)sw * 1.2 / (double)iw;
        double sy = (double)sh * 1.2 / (double)ih;
        if (sy > sc) sc = sy;
        int nw = (int)(iw * sc), nh = (int)(ih * sc);
        GpBitmap *dst = NULL;
        if (GdipCreateBitmapFromScan0(nw, nh, 0, PixelFormat32bppPARGB, NULL, &dst) == Ok && dst) {
            GpGraphics *g = NULL;
            if (GdipGetImageGraphicsContext((GpImage *)dst, &g) == Ok && g) {
                GdipSetInterpolationMode(g, InterpolationModeHighQualityBicubic);
                GdipDrawImageRectI(g, (GpImage *)src, 0, 0, nw, nh);
                GdipDeleteGraphics(g);
                GdipDisposeImage((GpImage *)src);
                g_bg_img = (GpImage *)dst;
                return 1;
            }
            GdipDisposeImage((GpImage *)dst);
        }
    }
    g_bg_img = (GpImage *)src;
    return 1;
}


#define BG_DIV 2   /* 背景按 1/2 分辨率渲染+合成，再拉伸到全屏：省 4 倍合成开销 */

static void render_background2(int w, int h, int slot, double glow_scale) {
    HDC *pdc = &g_bgdc[slot];
    HBITMAP *pbmp = &g_bgbmp[slot];
    if (*pdc) { DeleteDC(*pdc); *pdc = NULL; }
    if (*pbmp) { DeleteObject(*pbmp); *pbmp = NULL; }

    HDC screen = GetDC(NULL);
    *pdc = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);

    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    int bw = (w + BG_DIV - 1) / BG_DIV, bh2 = (h + BG_DIV - 1) / BG_DIV;
    bi.bmiHeader.biWidth = bw;
    bi.bmiHeader.biHeight = -bh2;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    *pbmp = CreateDIBSection(*pdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(*pdc, *pbmp);

    GpGraphics *g = NULL;
    GdipCreateFromHDC(*pdc, &g);
    if (!g) return;
    GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
    /* 之后所有绘制仍用全屏坐标，由世界变换缩到半分辨率 */
    GdipScaleWorldTransform(g, 1.0 / BG_DIV, 1.0 / BG_DIV, MatrixOrderAppend);

    RectF full = { 0, 0, (REAL)w, (REAL)h };

    if (g_bg_img) {
        UINT iw = 0, ih = 0;
        GdipGetImageWidth(g_bg_img, &iw);
        GdipGetImageHeight(g_bg_img, &ih);
        if (iw && ih) {
            double sx = (double)w / (double)iw, sy = (double)h / (double)ih;
            double sc = sx > sy ? sx : sy;
            float dw = (float)(iw * sc), dh = (float)(ih * sc);
            float dx = ((float)w - dw) / 2.0f, dy = ((float)h - dh) / 2.0f;
            GdipSetInterpolationMode(g, InterpolationModeHighQualityBicubic);
            GdipDrawImageRectI(g, g_bg_img, (INT)dx, (INT)dy, (INT)dw, (INT)dh);
            GdipSetInterpolationMode(g, InterpolationModeDefault);
        }
        int dark = (int)(120.0 + (1.45 - glow_scale) * 75.0);
        if (dark > 210) dark = 210;
        if (dark < 100) dark = 100;
        GpSolidFill *ov = NULL;
        GdipCreateSolidFill((ARGB)((((unsigned)dark) << 24) | 0x0A0E18u), &ov);
        if (ov) {
            GdipFillRectangle(g, (GpBrush *)ov, 0, 0, (REAL)w, (REAL)h);
            GdipDeleteBrush((GpBrush *)ov);
        }
    } else {
        GpLineGradient *lg = NULL;
        GdipCreateLineBrushFromRect(&full, (ARGB)0xFF141A30, (ARGB)0xFF090D18,
                                    LinearGradientModeForwardDiagonal, WrapModeTileFlipXY, &lg);
        if (lg) {
            GdipFillRectangle(g, (GpBrush *)lg, 0, 0, (REAL)w, (REAL)h);
            GdipDeleteBrush((GpBrush *)lg);
        }
    }

    if (!g_bg_img) {
    glow(g, w * 0.06f, h * 0.70f, w * 0.40f, 0xFF6B5A, (int)(11 * glow_scale));
    glow(g, w * 0.97f, h * 0.00f, w * 0.46f, 0x5B7BFF, (int)(15 * glow_scale));
    glow(g, w * 0.58f, h * 0.96f, w * 0.38f, 0x2FD3A0, (int)(7 * glow_scale));
    glow(g, w * 0.00f, h * 0.00f, w * 0.30f, 0x8E6BFF, (int)(9 * glow_scale));
    }

    GdipDeleteGraphics(g);
}

/* 合成结果缓存：alpha 量化到 8 的倍数，只有跨档时才重新 AlphaBlend */
static HDC g_bgc;
static HBITMAP g_bgcbmp;
static int g_bgc_w, g_bgc_h;

static void composite_bg(int w, int h, int alpha) {
    if (!g_bgdc[0] || !g_bgdc[1]) return;
    if (!g_bgc || w != g_bgc_w || h != g_bgc_h) {
        if (g_bgc) { DeleteDC(g_bgc); g_bgc = NULL; }
        if (g_bgcbmp) { DeleteObject(g_bgcbmp); g_bgcbmp = NULL; }
        HDC screen = GetDC(NULL);
        g_bgc = CreateCompatibleDC(screen);
        ReleaseDC(NULL, screen);
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        int bw2 = (w + BG_DIV - 1) / BG_DIV, bh3 = (h + BG_DIV - 1) / BG_DIV;
        bi.bmiHeader.biWidth = bw2;
        bi.bmiHeader.biHeight = -bh3;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void *bits = NULL;
        g_bgcbmp = CreateDIBSection(g_bgc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        SelectObject(g_bgc, g_bgcbmp);
        g_bgc_w = w;
        g_bgc_h = h;
        g_bgc_alpha = -1;
    }
    if (alpha == g_bgc_alpha) return;
    int bw = (w + BG_DIV - 1) / BG_DIV, bh2 = (h + BG_DIV - 1) / BG_DIV;
    BitBlt(g_bgc, 0, 0, bw, bh2, g_bgdc[0], 0, 0, SRCCOPY);
    BLENDFUNCTION bf;
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = (BYTE)alpha;
    bf.AlphaFormat = 0;
    AlphaBlend(g_bgc, 0, 0, bw, bh2, g_bgdc[1], 0, 0, bw, bh2, bf);
    g_bgc_alpha = alpha;
}

static void render_background(int w, int h) {
    if (w == g_bg_w && h == g_bg_h && g_bgdc[0] && g_bgdc[1]) return;
    render_background2(w, h, 0, 0.45);    /* 暗相：光晕 45% */
    render_background2(w, h, 1, 1.45);    /* 亮相：光晕 145% */
    g_bg_w = w;
    g_bg_h = h;
}

static void buffer_size(int w, int h) {
    if (w == g_buf_w && h == g_buf_h && g_memdc) return;
    if (g_gp) { GdipDeleteGraphics(g_gp); g_gp = NULL; }
    if (g_membmp) { DeleteObject(g_membmp); g_membmp = NULL; }
    if (g_memdc) { DeleteDC(g_memdc); g_memdc = NULL; }

    HDC screen = GetDC(NULL);
    g_memdc = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);

    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    g_membmp = CreateDIBSection(g_memdc, &bi, DIB_RGB_COLORS, &g_bits, NULL, 0);
    SelectObject(g_memdc, g_membmp);
    GdipCreateFromHDC(g_memdc, &g_gp);

    /* 平滑：抗锯齿 + 灰度抗锯齿文字（ClearType 在大字号暗底上会有彩色毛边） */
    GdipSetSmoothingMode(g_gp, SmoothingModeAntiAlias);
    GdipSetTextRenderingHint(g_gp, TextRenderingHintAntiAlias);
    GdipSetInterpolationMode(g_gp, InterpolationModeHighQualityBicubic);
    g_buf_w = w;
    g_buf_h = h;
}

/* ------------------------------------------------------------------ 绘制 */

static void fmt_seconds(double sec, wchar_t *out, int cap) {
    if (sec < 0) sec = 0;
    int total = (int)ceil(sec - 1e-9);
    int m = total / 60, s = total % 60;
    swprintf(out, (size_t)cap, L"%02d:%02d", m, s);
}

/* ---------------- 底部可点按钮 ---------------- */

enum { UI_RUN = 0, UI_RESET, UI_SKIP, UI_LIST, UI_EDIT, UI_QUIT, UI_COUNT };

typedef struct {
    int id;
    wchar_t label[24];
    float x, y, w, h;
    int primary;
} UiButton;

static UiButton g_ui[UI_COUNT];
static int g_scroll = 0;
static double g_btn_hover_t[UI_COUNT] = { 0 };
static int g_ui_ready = 0;
static int g_ui_hover = -1;
static int g_ui_press = -1;
static int g_ui_tracking = 0;

/* ---------------------------------------------------------------- 配色 */

typedef struct {
    unsigned paper, panel, ink, ink2, rule, chip, signal, onSignal, trace,
             tagBg, tagFg, danger, cmdBar, cmdText, scrim;
} Palette;

static Palette g_pal;
static int g_dark = 0;

static void palette_set(int dark) {
    g_dark = dark ? 1 : 0;
    if (!g_dark) {
        g_pal.paper = 0xF4F4F2; g_pal.panel = 0xFFFFFF; g_pal.ink = 0x161616;
        g_pal.ink2  = 0x5C5C5C; g_pal.rule  = 0xD9D9D6; g_pal.chip = 0xE4E4E0;
        g_pal.signal = 0xFFFA00; g_pal.onSignal = 0x161616; g_pal.trace = 0x161616;
        g_pal.tagBg = 0x161616; g_pal.tagFg = 0xFFFFFF; g_pal.danger = 0xB3261E;
        g_pal.cmdBar = 0x161616; g_pal.cmdText = 0xEDEDED; g_pal.scrim = 0x161616;
    } else {
        g_pal.paper = 0x191919; g_pal.panel = 0x222222; g_pal.ink = 0xEDEDED;
        g_pal.ink2  = 0xA3A3A3; g_pal.rule  = 0x353535; g_pal.chip = 0x2E2E2E;
        g_pal.signal = 0xFFFA00; g_pal.onSignal = 0x161616; g_pal.trace = 0xFFFA00;
        g_pal.tagBg = 0x3A3A3A; g_pal.tagFg = 0xEDEDED; g_pal.danger = 0xFF8A7A;
        g_pal.cmdBar = 0x161616; g_pal.cmdText = 0xEDEDED; g_pal.scrim = 0x000000;
    }
}

/* ---------------------------------------------------------------- 形状 */

typedef struct { float x, y, w, h; } Box;

/* corner: 0=左上 1=右上 2=右下 3=左下 */
static GpPath *cut_path(float x, float y, float w, float h, float cut, int corner) {
    if (cut < 0) cut = 0;
    if (cut > w / 2) cut = w / 2;
    if (cut > h / 2) cut = h / 2;
    GpPath *p = NULL;
    if (GdipCreatePath(FillModeAlternate, &p) != Ok || !p) return NULL;
    float x1 = x + w, y1 = y + h;
    switch (corner) {
    case 0:
        GdipAddPathLine(p, x + cut, y, x1, y);
        GdipAddPathLine(p, x1, y, x1, y1);
        GdipAddPathLine(p, x1, y1, x, y1);
        GdipAddPathLine(p, x, y1, x, y + cut);
        break;
    case 1:
        GdipAddPathLine(p, x, y, x1 - cut, y);
        GdipAddPathLine(p, x1 - cut, y, x1, y + cut);
        GdipAddPathLine(p, x1, y + cut, x1, y1);
        GdipAddPathLine(p, x1, y1, x, y1);
        GdipAddPathLine(p, x, y1, x, y);
        break;
    case 2:
        GdipAddPathLine(p, x, y, x1, y);
        GdipAddPathLine(p, x1, y, x1, y1 - cut);
        GdipAddPathLine(p, x1, y1 - cut, x1 - cut, y1);
        GdipAddPathLine(p, x1 - cut, y1, x, y1);
        GdipAddPathLine(p, x, y1, x, y);
        break;
    default:
        GdipAddPathLine(p, x, y, x1, y);
        GdipAddPathLine(p, x1, y, x1, y1);
        GdipAddPathLine(p, x1, y1, x + cut, y1);
        GdipAddPathLine(p, x + cut, y1, x, y1 - cut);
        GdipAddPathLine(p, x, y1 - cut, x, y);
        break;
    }
    GdipClosePathFigure(p);
    return p;
}

static void box_fill(GpGraphics *g, unsigned rgb, int a, float x, float y, float w, float h) {
    if (w <= 0 || h <= 0 || a <= 0) return;
    GpSolidFill *b = brush_new(argb(rgb, a));
    if (!b) return;
    GdipFillRectangle(g, (GpBrush *)b, x, y, w, h);
    GdipDeleteBrush((GpBrush *)b);
}

static void fill_cut(GpGraphics *g, unsigned rgb, int a, float x, float y, float w, float h,
                     float cut, int corner) {
    GpPath *p = cut_path(x, y, w, h, cut, corner);
    if (!p) return;
    GpSolidFill *b = brush_new(argb(rgb, a));
    if (b) {
        GdipFillPath(g, (GpBrush *)b, p);
        GdipDeleteBrush((GpBrush *)b);
    }
    GdipDeletePath(p);
}

static void stroke_cut(GpGraphics *g, unsigned rgb, int a, float x, float y, float w, float h,
                       float cut, int corner, float lw) {
    GpPath *p = cut_path(x + lw * 0.5f, y + lw * 0.5f, w - lw, h - lw, cut, corner);
    if (!p) return;
    GpPen *pen = NULL;
    GdipCreatePen1((ARGB)argb(rgb, a), lw, UnitPixel, &pen);
    if (pen) {
        GdipDrawPath(g, pen, p);
        GdipDeletePen(pen);
    }
    GdipDeletePath(p);
}

static void h_line(GpGraphics *g, unsigned rgb, int a, float x0, float x1, float y, float lw) {
    if (x1 <= x0) return;
    box_fill(g, rgb, a, x0, floorf(y), x1 - x0, lw);
}

static void v_line(GpGraphics *g, unsigned rgb, int a, float x, float y0, float y1, float lw) {
    if (y1 <= y0) return;
    box_fill(g, rgb, a, floorf(x), y0, lw, y1 - y0);
}

/* 45° 斜线纹：空状态、画框影子 */
static void hatch_box(GpGraphics *g, unsigned rgb, int a, float x, float y, float w, float h,
                      float period, float lw) {
    if (w <= 1 || h <= 2 || a <= 0) return;
    GdipSetClipRect(g, x, y, w, h, CombineModeReplace);
    GpPen *pen = NULL;
    GdipCreatePen1((ARGB)argb(rgb, a), lw, UnitPixel, &pen);
    if (pen) {
        float step = period * 1.4142f;
        for (float sx = x - h; sx < x + w + h; sx += step) {
            GdipDrawLine(g, pen, sx, y + h, sx + h, y);
        }
        GdipDeletePen(pen);
    }
    GdipResetClip(g);
}

static void tri_strip(GpGraphics *g, float x, float y, float w, float h) {
    float t = w / 3.0f;
    box_fill(g, 0xFF00F0, 255, x, y, t, h);
    box_fill(g, 0xFFFA00, 255, x + t, y, t, h);
    box_fill(g, 0x00FFA2, 255, x + t * 2, y, w - t * 2, h);
}

/* ---------------------------------------------------------------- 文字 */

static float text_w(GpGraphics *g, const wchar_t *s, GpFont *f) {
    if (!s || !s[0]) return 0;
    RectF layout = { 0, 0, 4000, 4000 }, bounds;
    INT cps = 0, lines = 0;
    if (GdipMeasureString(g, s, -1, f, &layout, NULL, &bounds, &cps, &lines) != Ok) return 0;
    return bounds.Width;
}

/* 以「竖直中心」定位一行字 */
static float text_cy(GpGraphics *g, const wchar_t *s, GpFont *f, unsigned rgb, int a,
                     float x, float cy, float w, int align, int size) {
    text_draw_a(g, s, f, rgb, a, x, cy - size * 0.62f, w, size * 1.24f, align);
    return text_w(g, s, f);
}

/* 伪粗体：Bahnschrift 是可变字体，GDI+ 只拿默认字重；用路径描边补厚度 */
static float text_cy_heavy(GpGraphics *g, const wchar_t *s, GpFont *f, unsigned rgb, int a,
                           float x, float cy, float w, int align, int size) {
    GpPath *path = NULL;
    GpStringFormat *fmt = NULL;
    RectF layout = { x, cy - size * 0.62f, w, size * 1.24f };
    if (GdipCreatePath(FillModeWinding, &path) == Ok && path &&
        GdipCreateStringFormat(0, LANG_NEUTRAL, &fmt) == Ok && fmt) {
        GdipSetStringFormatAlign(fmt, (StringAlignment)align);
        GdipSetStringFormatLineAlign(fmt, StringAlignmentCenter);
        GdipSetStringFormatFlags(fmt, StringFormatFlagsNoWrap);
        GpFontFamily *fam = g_family_latin ? g_family_latin : g_family_display;
        if (fam && GdipAddPathString(path, s, -1, fam, FontStyleRegular, (REAL)size,
                                     &layout, fmt) == Ok) {
            GpSolidFill *b = brush_new(argb(rgb, a));
            if (b) {
                GdipFillPath(g, (GpBrush *)b, path);
                GdipDeleteBrush((GpBrush *)b);
            }
            GpPen *pen = NULL;
            if (GdipCreatePen1((ARGB)argb(rgb, a), (REAL)(size * 0.020f), UnitPixel, &pen) == Ok && pen) {
                GdipSetPenLineJoin(pen, LineJoinRound);
                GdipDrawPath(g, pen, path);
                GdipDeletePen(pen);
            }
        }
        if (fmt) GdipDeleteStringFormat(fmt);
        GdipDeletePath(path);
        return text_w(g, s, f);
    }
    if (fmt) GdipDeleteStringFormat(fmt);
    if (path) GdipDeletePath(path);
    return text_cy(g, s, f, rgb, a, x, cy, w, align, size);
}

/* 带字距的宽体小字：GDI+ 不带 tracking，逐字摆 */
static void text_track(GpGraphics *g, const wchar_t *s, GpFont *f, unsigned rgb, int a,
                       float x, float cy, float track, int size) {
    float cx = x;
    for (const wchar_t *p = s; *p; p++) {
        wchar_t one[2] = { *p, 0 };
        text_draw_a(g, one, f, rgb, a, cx, cy - size * 0.62f, (float)size * 4.0f,
                    (float)size * 1.24f, 0);
        float adv = text_w(g, one, f);
        if (adv <= 0) adv = size * 0.45f;
        cx += adv + track;
    }
}

static float text_track_w(GpGraphics *g, const wchar_t *s, GpFont *f, float track) {
    float w = 0;
    for (const wchar_t *p = s; *p; p++) {
        wchar_t one[2] = { *p, 0 };
        float adv = text_w(g, one, f);
        if (adv <= 0) adv = 6;
        w += adv + track;
    }
    return w > 0 ? w - track : 0;
}

/* ---------------------------------------------------------------- 版式 */

typedef struct {
    float k, pad, w, h;
    float contentY, contentH, contentBottom, contentW;
    float eyebrowCy, titleCy, clockCy, clockSize, dateCy, ruleY;
    float deckY, deckH, dialCx, dialCy, dialR, g2X, g3X;
    float g2Cy, g3Cy;
    Box board;
    float headerH, rowH, footerH;
    int rows, showG3;
    float btnY, btnH;
    Box btn[UI_COUNT];
} Lay;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void layout_compute(Lay *L, int w, int h) {
    float k = clampf((float)h / 1080.0f, 0.60f, 1.45f);
    L->k = k;
    L->w = (float)w;
    L->h = (float)h;
    L->pad = floorf(48 * k + 0.5f);

    float stripH = 3, barH = floorf(54 * k + 0.5f), bottomH = floorf(92 * k + 0.5f);
    L->contentY = stripH + barH;
    L->contentH = (float)h - bottomH - L->contentY;
    L->contentBottom = (float)h - bottomH;
    L->contentW = (float)w - L->pad * 2;

    L->eyebrowCy = L->contentY + 50 * k;
    L->titleCy = L->eyebrowCy + 40 * k;
    L->clockSize = 300 * k;
    if (L->clockSize > L->contentW / 4.60f) L->clockSize = L->contentW / 4.60f;
    L->clockCy = L->titleCy + 30 * k + L->clockSize * 0.44f;
    L->dateCy = L->clockCy + L->clockSize * 0.40f + 62 * k;
    L->ruleY = L->dateCy + 30 * k;

    L->deckY = L->ruleY;
    L->deckH = L->contentBottom - L->ruleY;
    L->dialR = 130 * k;
    if (L->dialR > L->deckH * 0.30f) L->dialR = L->deckH * 0.30f;
    if (L->dialR < 48) L->dialR = 48;
    L->dialCx = L->pad + L->dialR;
    L->dialCy = L->deckY + L->deckH * 0.5f;
    L->g2X = L->pad + L->dialR * 2 + 60 * k;
    L->g3X = L->g2X + 360 * k;
    L->g2Cy = L->dialCy;
    L->g3Cy = L->dialCy;

    float boardW = L->contentW * 0.38f;
    if (boardW < 340) boardW = 340;
    if (boardW > 700 * k) boardW = 700 * k;
    L->board.x = (float)w - L->pad - boardW;
    L->board.y = L->deckY + 22 * k;
    L->board.w = boardW;
    L->board.h = L->contentBottom - L->board.y;
    L->showG3 = ((L->board.x - L->g3X) > 250 * k);

    L->headerH = 92 * k;
    L->footerH = 46 * k;
    int n = g_ntodos > 0 ? g_ntodos : 1;
    float room = L->board.h - L->headerH - L->footerH;
    int maxRows = (int)(room / (52 * k));
    if (maxRows < 1) maxRows = 1;
    L->rows = n < maxRows ? n : maxRows;
    float rh = room / (float)L->rows;
    if (rh < 52 * k) rh = 52 * k;
    if (rh > 68 * k) rh = 68 * k;
    L->rowH = rh;

    L->btnH = floorf(42 * k + 0.5f);
    L->btnY = L->contentBottom + 26 * k;
    static const float widths[UI_COUNT] = { 132, 96, 96, 116, 96, 84 };
    float x = L->pad;
    for (int i = 0; i < UI_COUNT; i++) {
        float bw = floorf(widths[i] * k + 0.5f);
        L->btn[i].x = x;
        L->btn[i].y = L->btnY;
        L->btn[i].w = bw;
        L->btn[i].h = L->btnH;
        x += bw + (i == UI_RUN ? 20 * k : 14 * k);
    }
}

static void btn_labels(void) {
    wcscpy(g_ui[UI_RUN].label, g_pomo.running ? L"暂停" : L"开始");
    wcscpy(g_ui[UI_RESET].label, L"重置");
    wcscpy(g_ui[UI_SKIP].label, L"跳过");
    wcscpy(g_ui[UI_LIST].label, g_list_pinned ? L"收起待办" : L"待办");
    wcscpy(g_ui[UI_EDIT].label, L"设置");
    wcscpy(g_ui[UI_QUIT].label, L"退出");
}

static void ui_layout(int w, int h) {
    Lay L;
    layout_compute(&L, w, h);
    for (int i = 0; i < UI_COUNT; i++) {
        g_ui[i].id = i;
        g_ui[i].x = L.btn[i].x;
        g_ui[i].y = L.btn[i].y;
        g_ui[i].w = L.btn[i].w;
        g_ui[i].h = L.btn[i].h;
        g_ui[i].primary = (i == UI_RUN);
    }
    btn_labels();
    g_ui_ready = 1;
}

static int ui_hit(int mx, int my) {
    if (!g_ui_ready) return -1;
    for (int i = 0; i < UI_COUNT; i++) {
        if (mx >= g_ui[i].x && mx <= g_ui[i].x + g_ui[i].w &&
            my >= g_ui[i].y && my <= g_ui[i].y + g_ui[i].h)
            return i;
    }
    return -1;
}

static int g_row_press = -1;

/* 点在待办看板的某一行上？返回条目下标，否则 -1 */
static int hit_row(int mx, int my, int w, int h) {
    if (g_list_alpha <= 0.5 || g_ntodos <= 0) return -1;
    Lay L;
    layout_compute(&L, w, h);
    if (mx < L.board.x || mx > L.board.x + L.board.w) return -1;
    float top = L.board.y + L.headerH;
    if (my < top || my >= top + L.rows * L.rowH) return -1;
    int r = (int)((my - top) / L.rowH);
    int i = g_scroll + r;
    return (i >= 0 && i < g_ntodos) ? i : -1;
}

/* 点在圆盘上？ */
static int hit_dial(int mx, int my, int w, int h) {
    Lay L;
    layout_compute(&L, w, h);
    float dx = mx - L.dialCx, dy = my - L.dialCy;
    float rr = L.dialR * 1.22f;
    return (dx * dx + dy * dy) <= rr * rr;
}

/* ---------------- 选择背景图 / 应用背景 / 待办常驻 ---------------- */

static int pick_image(wchar_t *out, int cap) {
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    wchar_t buf[MAX_PATH] = { 0 };
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_editor ? g_editor : g_hwnd;
    ofn.lpstrFilter = L"图片 (*.jpg;*.jpeg;*.png;*.bmp;*.gif)\0*.jpg;*.jpeg;*.png;*.bmp;*.gif\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"选择背景图片";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return 0;
    wcsncpy(out, buf, (size_t)cap - 1);
    out[cap - 1] = 0;
    return 1;
}

static void bg_apply(const wchar_t *path) {
    if (path && path[0]) {
        if (!bg_image_load(path)) {
            MessageBoxW(g_editor ? g_editor : g_hwnd, L"这个图片读不了，换一张试试。",
                        L"背景图片", MB_ICONWARNING);
            return;
        }
        wcsncpy(g_bg_image, path, MAX_PATH - 1);
        g_bg_image[MAX_PATH - 1] = 0;
    } else {
        g_bg_image[0] = 0;
        bg_image_free();
    }
    settings_save();
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, TRUE);
}

static void list_toggle_pin(void) {
    g_list_pinned = !g_list_pinned;
    if (g_list_pinned) g_list_clock = 0;
    else g_list_clock = (double)g_list_dur;
}

static unsigned shade(unsigned rgb, double f);

static Phase pomo_next_phase(const Pomodoro *p) {
    if (p->phase == PHASE_WORK)
        return ((p->completed + 1) % p->long_every == 0) ? PHASE_LONG : PHASE_SHORT;
    return PHASE_WORK;
}

static int pomo_next_minutes(const Pomodoro *p) {
    Phase n = pomo_next_phase(p);
    if (n == PHASE_WORK) return p->work_min;
    if (n == PHASE_SHORT) return p->short_min;
    return p->long_min;
}

static int g_hover_row = -1;


/* ---------------------------------------------------------------- 日期 */

static const wchar_t *kWeekday[] = { L"星期日", L"星期一", L"星期二", L"星期三",
                                     L"星期四", L"星期五", L"星期六" };

static int day_of_week(int y, int m, int d) {
    /* 0 = 周一 … 6 = 周日 */
    if (m < 3) { y -= 1; m += 12; }
    int c = y / 100, yy = y % 100;
    int w = (yy + yy / 4 + c / 4 - 2 * c + 26 * (m + 1) / 10 + d - 1) % 7;
    if (w < 0) w += 7;
    return w;
}

static int iso_week(int y, int m, int d) {
    static const int cum[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    int doy = cum[m - 1] + d;
    int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    if (leap && m > 2) doy++;
    int wd = day_of_week(y, 1, 1);
    int week = (doy - 1 + wd) / 7 + 1;
    if (week < 1) week = 1;
    if (week > 53) week = 53;
    return week;
}

static void utc_label(wchar_t *out, int cap) {
    TIME_ZONE_INFORMATION tzi;
    DWORD r = GetTimeZoneInformation(&tzi);
    int bias = tzi.Bias;
    if (r == TIME_ZONE_ID_DAYLIGHT) bias += tzi.DaylightBias;
    else if (r == TIME_ZONE_ID_STANDARD) bias += tzi.StandardBias;
    int off = -bias;                       /* 分钟，东八区 = +480 */
    int hh = off / 60, mm = off % 60;
    if (mm < 0) { mm = -mm; }
    swprintf(out, (size_t)cap, L"UTC%+03d:%02d", hh, mm);
}

/* ---------------------------------------------------------------- 背景 */

static void draw_wallpaper(GpGraphics *g, float w, float h) {
    if (!g_bg_img) return;
    UINT iw = 0, ih = 0;
    GdipGetImageWidth(g_bg_img, &iw);
    GdipGetImageHeight(g_bg_img, &ih);
    if (!iw || !ih) return;
    float sx = w / (float)iw, sy = h / (float)ih;
    float sc = sx > sy ? sx : sy;
    float dw = iw * sc, dh = ih * sc;
    float dx = (w - dw) * 0.5f, dy = (h - dh) * 0.5f;

    GpImageAttributes *ia = NULL;
    if (GdipCreateImageAttributes(&ia) != Ok || !ia) return;
    float a = g_dark ? 0.16f : 0.12f;
    ColorMatrix cm;
    memset(&cm, 0, sizeof cm);
    REAL *cmv = (REAL *)&cm;          /* 5×5 连续存放，与结构体成员名无关 */
    cmv[0] = 1; cmv[6] = 1; cmv[12] = 1; cmv[18] = a; cmv[24] = 1;
    GdipSetImageAttributesColorMatrix(ia, ColorAdjustTypeBitmap, TRUE, &cm, NULL,
                                      ColorMatrixFlagsDefault);
    GdipDrawImageRectRect(g, g_bg_img, dx, dy, dw, dh, 0, 0,
                          (REAL)iw, (REAL)ih, UnitPixel, ia, NULL, NULL);
    GdipDisposeImageAttributes(ia);
}

static void draw_contours(GpGraphics *g, Lay *L) {
    GpPen *pen = NULL;
    GdipCreatePen1((ARGB)argb(g_pal.ink, g_dark ? 18 : 24), 1.0f, UnitPixel, &pen);
    if (!pen) return;
    float cx = L->w - L->pad + 26 * L->k;
    float cy = L->contentY + 40 * L->k;
    float radii[] = { 96, 138, 180, 222, 264 };
    for (int i = 0; i < 5; i++) {
        float r = radii[i] * L->k;
        float a0 = (i < 3) ? 96.0f : 120.0f;
        float a1 = (i < 3) ? 276.0f : 238.0f;
        GdipDrawArc(g, pen, cx - r, cy - r, r * 2, r * 2, a0, a1 - a0);
    }
    GdipDeletePen(pen);
}

/* ---------------------------------------------------------------- 命令栏 */

static void draw_command_bar(GpGraphics *g, Lay *L) {
    float k = L->k;
    float barH = 54 * k;
    box_fill(g, g_pal.cmdBar, 255, 0, 3, L->w, barH);
    if (g_dark) h_line(g, g_pal.rule, 150, 0, L->w, 3 + barH - 1, 1);
    tri_strip(g, 0, 0, L->w, 3);

    float mid = 3 + barH * 0.5f;
    GpFont *fBrand = font_get((int)(15 * k), FONT_UI_BOLD);
    GpFont *fEye = font_get((int)(11 * k), FONT_LATIN);
    GpFont *fField = font_get((int)(13 * k), FONT_LATIN);

    box_fill(g, g_pal.signal, 255, L->pad, mid - 4.5f * k, 9 * k, 9 * k);
    float bx = L->pad + 18 * k;
    float eyeW = text_track_w(g, L"// POMODORO", fEye, 11 * k * 0.14f);
    text_track(g, L"// POMODORO", fEye, g_pal.cmdText, 150, bx, mid, 11 * k * 0.14f, (int)(11 * k));
    text_cy(g, L"番茄钟", fBrand, g_pal.cmdText, 255, bx + eyeW + 14 * k, mid, 220 * k, 0, (int)(15 * k));

    /* 中间：三段时长 */
    wchar_t seg[3][32];
    swprintf(seg[0], 32, L"专注 %d", g_pomo.work_min);
    swprintf(seg[1], 32, L"短休 %d", g_pomo.short_min);
    swprintf(seg[2], 32, L"长休 %d", g_pomo.long_min);
    float total = 0;
    for (int i = 0; i < 3; i++) {
        total += text_w(g, seg[i], fBrand) + 18 * k;
        if (i < 2) total += text_w(g, L"│", fField) + 18 * k;
    }
    float cx = (L->w - total) * 0.5f;
    for (int i = 0; i < 3; i++) {
        cx += text_cy(g, seg[i], fBrand, g_pal.cmdText, 160, cx, mid, 300, 0, (int)(13 * k)) + 18 * k;
        if (i < 2) {
            text_cy(g, L"│", fField, g_pal.cmdText, 45, cx, mid, 20, 0, (int)(13 * k));
            cx += text_w(g, L"│", fField) + 18 * k;
        }
    }

    /* 右：第几组 + 状态 */
    wchar_t st[64];
    swprintf(st, 64, L"第 %d 组 / %d", g_pomo.completed % g_pomo.long_every + 1, g_pomo.long_every);
    float rx = L->w - L->pad;
    float w1 = text_w(g, g_pomo.running ? L"运行中" : L"已暂停", fBrand);
    text_cy(g, g_pomo.running ? L"运行中" : L"已暂停", fBrand,
            g_pomo.running ? g_pal.signal : g_pal.cmdText, g_pomo.running ? 255 : 150,
            rx - w1, mid, 160, 0, (int)(13 * k));
    if (g_pomo.running) box_fill(g, g_pal.signal, 255, rx - w1 - 13 * k, mid - 3.5f * k, 7 * k, 7 * k);
    rx -= w1 + 21 * k;
    float w2 = text_w(g, st, fBrand);
    text_cy(g, st, fBrand, g_pal.cmdText, 150, rx - w2, mid, 200, 0, (int)(13 * k));
    rx -= w2 + 18 * k;
    text_cy(g, L"│", fField, g_pal.cmdText, 45, rx, mid, 20, 0, (int)(13 * k));
}

/* ---------------------------------------------------------------- 时钟 */

static void draw_clock_banner(GpGraphics *g, Lay *L) {
    float k = L->k;
    SYSTEMTIME st;
    GetLocalTime(&st);

    GpFont *fEye = font_get((int)(11 * k), FONT_LATIN);
    GpFont *fTitle = font_get((int)(24 * k), FONT_UI_BOLD);
    GpFont *fClock = font_get((int)L->clockSize, FONT_DISPLAY);
    GpFont *fField = font_get((int)(15 * k), FONT_LATIN);
    GpFont *fZh = font_get((int)(15 * k), FONT_UI);

    float x0 = L->pad;
    float track = 11 * k * 0.14f;
    text_track(g, L"// LOCAL TIME", fEye, g_pal.ink2, 255, x0, L->eyebrowCy, track, (int)(11 * k));
    text_cy(g, L"本地时间", fTitle, g_pal.ink, 255, x0, L->titleCy, 300 * k, 0, (int)(24 * k));

    wchar_t clock[32];
    swprintf(clock, 32, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    text_cy_heavy(g, clock, fClock, g_pal.ink, 255, x0 - L->clockSize * 0.03f, L->clockCy,
                  L->w, 0, (int)L->clockSize);

    /* 日期字段行 */
    wchar_t date[32], wk[32], wkn[32], utc[32];
    swprintf(date, 32, L"%04d.%02d.%02d", st.wYear, st.wMonth, st.wDay);
    wcscpy(wk, kWeekday[st.wDayOfWeek % 7]);
    swprintf(wkn, 32, L"W%d", iso_week(st.wYear, st.wMonth, st.wDay));
    utc_label(utc, 32);

    float cx = x0;
    cx += text_cy(g, date, fField, g_pal.ink2, 255, cx, L->dateCy, 400, 0, (int)(15 * k)) + 14 * k;
    text_cy(g, L"│", fField, g_pal.ink2, 90, cx, L->dateCy, 20, 0, (int)(15 * k));
    cx += text_w(g, L"│", fField) + 14 * k;
    cx += text_cy(g, wk, fZh, g_pal.ink2, 255, cx, L->dateCy, 200, 0, (int)(15 * k)) + 14 * k;
    text_cy(g, L"│", fField, g_pal.ink2, 90, cx, L->dateCy, 20, 0, (int)(15 * k));
    cx += text_w(g, L"│", fField) + 14 * k;
    text_cy(g, wkn, fField, g_pal.ink2, 255, cx, L->dateCy, 200, 0, (int)(15 * k));

    float uw = text_w(g, utc, fField);
    text_cy(g, utc, fField, g_pal.ink2, 200, L->w - L->pad - uw, L->dateCy, 200, 0, (int)(15 * k));
}

/* ---------------------------------------------------------------- 仪表盘 */

static void draw_deck(GpGraphics *g, Lay *L) {
    float k = L->k;
    float cx = L->dialCx, cy = L->dialCy, r = L->dialR;
    float thick = 18 * k;
    if (thick < 10) thick = 10;

    /* 外框细线 */
    GpPen *ring = NULL;
    GdipCreatePen1((ARGB)argb(g_pal.rule, 255), 1.0f, UnitPixel, &ring);
    if (ring) {
        float rr = r + thick * 0.5f + 9 * k;
        GdipDrawEllipse(g, ring, cx - rr, cy - rr, rr * 2, rr * 2);
        GdipDeletePen(ring);
    }

    /* 60 格刻度环 */
    double frac = g_pomo.total > 0 ? g_pomo.remaining / g_pomo.total : 0;
    double gone = 1.0 - frac;
    int head = (int)(gone * 60.0);
    if (head < 0) head = 0;
    if (head > 59) head = 59;
    for (int i = 0; i < 60; i++) {
        unsigned col = (i < head) ? g_pal.ink : (i == head ? g_pal.signal : g_pal.chip);
        GpPen *pen = NULL;
        GdipCreatePen1((ARGB)argb(col, 255), thick, UnitPixel, &pen);
        if (!pen) continue;
        float a0 = -90.0f + i * 6.0f + 0.45f;
        GdipDrawArc(g, pen, cx - r, cy - r, r * 2, r * 2, a0, 4.65f);
        GdipDeletePen(pen);
    }

    /* 环内读数 */
    GpFont *fPhase = font_get((int)(12.5f * k), FONT_UI_BOLD);
    float tsz = 52 * k;
    if (tsz > r * 0.42f) tsz = r * 0.42f;
    GpFont *fTime = font_get((int)tsz, FONT_DISPLAY);
    GpFont *fSmall = font_get((int)(11 * k), FONT_UI);
    text_cy(g, phase_name_plain(g_pomo.phase), fPhase, g_pal.ink2, 255, cx - r, cy - r * 0.36f, r * 2, 1, (int)(12.5f * k));
    wchar_t remain[16];
    fmt_seconds(g_pomo.remaining, remain, 16);
    text_cy_heavy(g, remain, fTime, g_pal.ink, 255, cx - r, cy + r * 0.17f, r * 2, 1, (int)tsz);
    text_cy(g, g_pomo.running ? L"进行中" : L"已暂停", fSmall,
            g_pomo.running ? g_pal.ink2 : shade(g_pal.ink2, 0.8f), 255,
            cx - r, cy + r * 0.52f, r * 2, 1, (int)(11 * k));

    /* 竖线分组 */
    float rt = cy - r * 0.82f, rb = cy + r * 0.82f;
    v_line(g, g_pal.rule, 255, L->g2X - 30 * k, rt, rb, 1);
    if (L->showG3) v_line(g, g_pal.rule, 255, L->g3X - 30 * k, rt, rb, 1);

    GpFont *fEye = font_get((int)(11 * k), FONT_LATIN);
    GpFont *fTitle = font_get((int)(22 * k), FONT_UI_BOLD);
    GpFont *fField = font_get((int)(14 * k), FONT_LATIN);
    GpFont *fZh13 = font_get((int)(13 * k), FONT_UI);
    float track = 11 * k * 0.14f;

    /* 本轮进度 */
    text_track(g, L"// SESSIONS", fEye, g_pal.ink2, 255, L->g2X, cy - 44 * k, track, (int)(11 * k));
    text_cy(g, L"本轮进度", fTitle, g_pal.ink, 255, L->g2X, cy - 10 * k, 300 * k, 0, (int)(22 * k));

    int done = g_pomo.completed % g_pomo.long_every;
    if (g_pomo.phase == PHASE_WORK && g_pomo.completed > 0 && done == 0) done = g_pomo.long_every;
    float bw = 56 * k, bh = 14 * k, gap = 12 * k;
    float by = cy + 14 * k;
    for (int i = 0; i < g_pomo.long_every; i++) {
        int active = (i == done && g_pomo.phase == PHASE_WORK);
        unsigned col = (i < done) ? g_pal.ink : (active ? g_pal.signal : g_pal.chip);
        box_fill(g, col, 255, L->g2X + i * (bw + gap), by, bw, bh);
        if (active) stroke_cut(g, g_pal.ink, 255, L->g2X + i * (bw + gap), by, bw, bh, 0, 2, 1);
    }
    wchar_t cnt[32];
    swprintf(cnt, 32, L"%d / %d", done, g_pomo.long_every);
    float fx = L->g2X;
    fx += text_cy(g, cnt, fField, g_pal.ink2, 255, fx, by + bh + 28 * k, 100, 0, (int)(14 * k)) + 12 * k;
    text_cy(g, L"│", fField, g_pal.ink2, 90, fx, by + bh + 28 * k, 20, 0, (int)(14 * k));
    fx += text_w(g, L"│", fField) + 12 * k;
    wchar_t per[32];
    swprintf(per, 32, L"%d 分钟一组", g_pomo.work_min);
    text_cy(g, per, fZh13, g_pal.ink2, 255, fx, by + bh + 28 * k, 200, 0, (int)(13 * k));

    if (!L->showG3) return;

    /* 下一次 */
    text_track(g, L"// NEXT", fEye, g_pal.ink2, 255, L->g3X, cy - 44 * k, track, (int)(11 * k));
    wchar_t nxt[64];
    swprintf(nxt, 64, L"下次 %ls", phase_name_plain(pomo_next_phase(&g_pomo)));
    text_cy(g, nxt, fTitle, g_pal.ink, 255, L->g3X, cy - 10 * k, 360 * k, 0, (int)(22 * k));
    wchar_t mins[32];
    swprintf(mins, 32, L"%d 分钟", pomo_next_minutes(&g_pomo));
    text_cy(g, mins, fZh13, g_pal.ink2, 255, L->g3X, cy + 26 * k, 200, 0, (int)(13 * k));
    wchar_t acc[64];
    swprintf(acc, 64, L"已累计 %d 分钟", g_pomo.completed * g_pomo.work_min);
    text_cy(g, acc, fZh13, g_pal.ink2, 220, L->g3X, cy + 54 * k, 300, 0, (int)(13 * k));
}

/* ---------------------------------------------------------------- 待办看板 */

static void board_row_box(Lay *L, int r, Box *out) {
    out->x = L->board.x;
    out->y = L->board.y + L->headerH + r * L->rowH;
    out->w = L->board.w;
    out->h = L->rowH;
}

static Box board_check_box(Lay *L, int r, float shift) {
    Box row, cb;
    board_row_box(L, r, &row);
    cb.x = L->board.x + 24 * L->k + shift;
    cb.y = row.y + row.h * 0.5f - 9 * L->k;
    cb.w = 18 * L->k;
    cb.h = 18 * L->k;
    return cb;
}

static void draw_check(GpGraphics *g, Box b, int checked, int hot, float k) {
    float cut = 5 * k;
    if (checked) {
        fill_cut(g, hot ? g_pal.ink : g_pal.tagBg, 255, b.x, b.y, b.w, b.h, cut, 2);
        GpPen *pen = NULL;
        GdipCreatePen1((ARGB)argb(g_dark ? g_pal.paper : g_pal.panel, 245), 2.2f * k, UnitPixel, &pen);
        if (pen) {
            GdipDrawLine(g, pen, b.x + b.w * 0.24f, b.y + b.h * 0.52f,
                         b.x + b.w * 0.44f, b.y + b.h * 0.76f);
            GdipDrawLine(g, pen, b.x + b.w * 0.44f, b.y + b.h * 0.76f,
                         b.x + b.w * 0.78f, b.y + b.h * 0.26f);
            GdipDeletePen(pen);
        }
    } else {
        stroke_cut(g, hot ? g_pal.ink : g_pal.ink, hot ? 255 : 128, b.x, b.y, b.w, b.h, cut, 2, 1);
    }
}

static void draw_tag(GpGraphics *g, Box b, const wchar_t *s, int current, float k) {
    fill_cut(g, current ? g_pal.signal : g_pal.tagBg, 255, b.x, b.y, b.w, b.h, 7 * k, 2);
    GpFont *f = font_get((int)(b.h * 0.56f), FONT_LATIN);
    text_cy(g, s, f, current ? g_pal.onSignal : g_pal.tagFg, 255, b.x + 4 * k,
            b.y + b.h * 0.5f, b.w - 8 * k, 1, (int)(b.h * 0.56f));
}

static void draw_board(GpGraphics *g, Lay *L) {
    float k = L->k;
    Box b = L->board;
    float cut = 26 * k;

    /* 画框影子：硬边斜线纹 */
    hatch_box(g, g_pal.ink, g_dark ? 26 : 28, b.x + 9 * k, b.y + 9 * k, b.w, b.h, 7 * k, 1.5f * k);

    fill_cut(g, g_pal.panel, 255, b.x, b.y, b.w, b.h, cut, 0);
    stroke_cut(g, g_pal.ink, 255, b.x, b.y, b.w, b.h, cut, 0, 1);
    box_fill(g, g_pal.ink, 255, b.x + cut, b.y, b.w * 0.38f, 5 * k);

    float x0 = b.x + 26 * k, x1 = b.x + b.w - 24 * k;
    GpFont *fEye = font_get((int)(11 * k), FONT_LATIN);
    GpFont *fTitle = font_get((int)(22 * k), FONT_UI_BOLD);
    GpFont *fCount = font_get((int)(15 * k), FONT_LATIN);
    float track = 11 * k * 0.14f;

    text_track(g, L"// TASKS", fEye, g_pal.ink2, 255, x0, b.y + 32 * k, track, (int)(11 * k));
    text_cy(g, L"待办", fTitle, g_pal.ink, 255, x0, b.y + 62 * k, 200 * k, 0, (int)(22 * k));
    wchar_t cnt[32];
    swprintf(cnt, 32, L"%02d / %02d", todo_done_count(), g_ntodos);
    text_cy(g, cnt, fCount, g_pal.ink2, 255, x1 - 160 * k, b.y + 62 * k, 160 * k, 2, (int)(15 * k));
    h_line(g, g_pal.rule, 255, x0, x1, b.y + L->headerH - 14 * k, 1);

    int live = (g_list_alpha > 0.02);
    float top = b.y + L->headerH;

    if (!live) {
        /* 静默：斜线纹 + 提示 */
        float boxH = 46 * k;
        float boxY = b.y + b.h - L->footerH - 18 * k - boxH;
        float bandY = top + 10 * k;
        float bandH = boxY - 22 * k - bandY;
        if (bandH < 30 * k) bandH = 30 * k;
        hatch_box(g, g_pal.ink, g_dark ? 22 : 16, x0, bandY, x1 - x0, bandH, 7 * k, 1.5f * k);

        box_fill(g, g_pal.panel, 255, x0, boxY, x1 - x0, boxH);
        stroke_cut(g, g_pal.rule, 255, x0, boxY, x1 - x0, boxH, 0, 2, 1);
        box_fill(g, g_pal.signal, 255, x0 + 14 * k, boxY + boxH * 0.5f - 7 * k, 3 * k, 14 * k);
        GpFont *fHint = font_get((int)(13 * k), FONT_UI);
        wchar_t msg[128];
        if (g_ntodos == 0) wcscpy(msg, L"还没有待办。按 E 打开设置，写下第一条。");
        else swprintf(msg, 128, L"待办每 %d 秒出现一次，按 L 常驻。", g_list_every);
        text_cy(g, msg, fHint, g_pal.ink2, 255, x0 + 28 * k, boxY + boxH * 0.5f,
                x1 - x0 - 40 * k, 0, (int)(13 * k));
    } else {
        int first = g_scroll;
        if (first > g_ntodos - 1) first = 0;
        for (int r = 0; r < L->rows; r++) {
            int i = first + r;
            if (i >= g_ntodos) break;
            double p = (g_list_alpha - r * 0.055) / 0.62;
            if (p < 0) p = 0;
            if (p > 1) p = 1;
            if (p <= 0.01) continue;
            int a = (int)(p * 255 * (g_list_alpha > 1 ? 1 : g_list_alpha * 1.15));
            if (a > 255) a = 255;

            Box row;
            board_row_box(L, r, &row);
            int sel = (i == g_todo_sel) && g_list_pinned;
            float shift = (sel ? 6 * k : 0) + (1 - (float)p) * 22 * k;

            if (sel) box_fill(g, g_pal.trace, 255, b.x + 1, row.y + 1, 4 * k, row.h - 2);
            else if (g_ui_hover == -1 && i == g_hover_row)
                box_fill(g, g_pal.chip, 140, b.x + 12 * k, row.y + 3 * k, row.w - 24 * k, row.h - 6 * k);

            Box cb = board_check_box(L, r, shift);
            draw_check(g, cb, g_todos[i].done, 0, k);

            Box tag;
            tag.x = cb.x + cb.w + 14 * k;
            tag.y = row.y + row.h * 0.5f - 9.5f * k;
            tag.w = 32 * k;
            tag.h = 19 * k;
            wchar_t num[8];
            swprintf(num, 8, L"%02d", i + 1);
            draw_tag(g, tag, num, sel, k);

            GpFont *fItem = font_get((int)(15 * k), FONT_UI);
            float tx = tag.x + tag.w + 14 * k;
            float tw = (b.x + b.w - 24 * k) - tx;
            unsigned col = g_todos[i].done ? g_pal.ink2 : g_pal.ink;
            text_cy(g, g_todos[i].text, fItem, col, g_todos[i].done ? 230 : 255,
                    tx, row.y + row.h * 0.5f, tw, 0, (int)(15 * k));

            if (g_todos[i].done) {
                float wpx = text_w(g, g_todos[i].text, fItem);
                if (wpx > tw) wpx = tw;
                h_line(g, g_pal.ink2, 170, tx, tx + wpx, row.y + row.h * 0.5f, 1.2f * k);
            }

            if (i < g_ntodos - 1)
                h_line(g, g_pal.rule, 255, b.x + 24 * k, b.x + b.w - 24 * k, row.y + row.h, 1);
        }
    }

    /* 页脚 */
    GpFont *fFoot = font_get((int)(11.5f * k), FONT_UI);
    float footCy = b.y + b.h - 18 * k;
    int over = g_ntodos - L->rows;
    if (over > 0) {
        wchar_t s[64];
        swprintf(s, 64, L"还有 %d 条  ·  滚轮翻页", over);
        text_cy(g, s, fFoot, g_pal.ink2, 220, x0, footCy, 300 * k, 0, (int)(11.5f * k));
        text_cy(g, L"E 打开编辑", fFoot, g_pal.ink2, 220, x1 - 200 * k, footCy, 200 * k, 2, (int)(11.5f * k));
    } else {
        wchar_t s[64];
        if (g_ntodos == 0) wcscpy(s, L"还没有条目");
        else swprintf(s, 64, L"共 %d 条", g_ntodos);
        text_cy(g, s, fFoot, g_pal.ink2, 200, x0, footCy, 300 * k, 0, (int)(11.5f * k));
        text_cy(g, L"E 打开设置", fFoot, g_pal.ink2, 200, x1 - 200 * k, footCy, 200 * k, 2, (int)(11.5f * k));
    }
}

/* ---------------------------------------------------------------- 动作条 */

static void draw_button(GpGraphics *g, Lay *L, int i) {
    float k = L->k;
    Box b = L->btn[i];
    int primary = (i == UI_RUN);
    int toggled = (i == UI_LIST && g_list_pinned);
    int press = (g_ui_press == i);
    float hov = (float)g_btn_hover_t[i];
    float cut = 8 * k;
    float y = b.y + (press ? 1.0f : 0.0f);

    if (primary) fill_cut(g, g_pal.ink, 255, b.x, y, b.w, b.h, cut, 2);
    else if (toggled) fill_cut(g, g_pal.chip, 255, b.x, y, b.w, b.h, cut, 2);
    else fill_cut(g, g_pal.panel, 255, b.x, y, b.w, b.h, cut, 2);

    if (hov > 0.001f) {
        GpPath *p = cut_path(b.x, y, b.w, b.h, cut, 2);
        if (p && GdipSetClipPath(g, p, CombineModeReplace) == Ok) {
            box_fill(g, g_pal.signal, 255, b.x, y, b.w * hov + 2, b.h);
            GdipResetClip(g);
        }
        if (p) GdipDeletePath(p);
    }

    if (primary) stroke_cut(g, g_pal.ink, 255, b.x, y, b.w, b.h, cut, 2, 1);
    else stroke_cut(g, g_pal.ink, toggled ? 255 : 140, b.x, y, b.w, b.h, cut, 2, 1);

    unsigned col = (hov > 0.45f) ? g_pal.onSignal : (primary ? g_pal.paper : g_pal.ink);
    GpFont *f = font_get((int)(15 * k), FONT_UI_BOLD);
    text_cy(g, g_ui[i].label, f, col, 255, b.x, y + b.h * 0.5f, b.w, 1, (int)(15 * k));
}

static void draw_action_bar(GpGraphics *g, Lay *L) {
    float k = L->k;
    h_line(g, g_pal.rule, 255, 0, L->w, L->contentBottom, 1);
    for (int i = 0; i < UI_COUNT; i++) draw_button(g, L, i);

    if (L->w < 1400 * k) return;
    GpFont *f = font_get((int)(12 * k), FONT_UI);
    const wchar_t *hint = L"空格 开始   R 重置   N 跳过   L 待办   E 设置   T 透明度   D 深浅   F11 窗口";
    float hw = text_w(g, hint, f);
    text_cy(g, hint, f, g_pal.ink2, 220, L->w - L->pad - hw, L->btnY + L->btnH * 0.5f, hw, 0, (int)(12 * k));
}

/* ---------------------------------------------------------------- 离屏渲染 */

static void paint_ui(GpGraphics *g, int w, int h);

static int render_png(const wchar_t *path, int w, int h) {
    GpBitmap *bmp = NULL;
    if (GdipCreateBitmapFromScan0(w, h, 0, PixelFormat32bppPARGB, NULL, &bmp) != Ok || !bmp) return 0;
    GpGraphics *g = NULL;
    if (GdipGetImageGraphicsContext((GpImage *)bmp, &g) != Ok || !g) {
        GdipDisposeImage((GpImage *)bmp);
        return 0;
    }
    GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
    GdipSetTextRenderingHint(g, TextRenderingHintAntiAlias);
    GdipSetInterpolationMode(g, InterpolationModeHighQualityBicubic);
    GdipSetPixelOffsetMode(g, PixelOffsetModeHalf);

    paint_ui(g, w, h);

    GdipDeleteGraphics(g);
    CLSID png;
    CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &png);
    int ok = (GdipSaveImageToFile((GpImage *)bmp, path, &png, NULL) == Ok);
    GdipDisposeImage((GpImage *)bmp);
    return ok;
}

/* ---------------------------------------------------------------- 主绘制 */

static void paint_ui(GpGraphics *g, int w, int h) {
    Lay L;
    layout_compute(&L, w, h);
    btn_labels();

    box_fill(g, g_pal.paper, 255, 0, 0, (float)w, (float)h);
    draw_wallpaper(g, (float)w, (float)h);
    draw_contours(g, &L);
    draw_command_bar(g, &L);
    draw_clock_banner(g, &L);
    draw_deck(g, &L);
    draw_board(g, &L);
    draw_action_bar(g, &L);
}

static void paint(HDC target, int w, int h) {
    buffer_size(w, h);
    GpGraphics *g = g_gp;
    if (!g) return;
    paint_ui(g, w, h);
    BitBlt(target, 0, 0, w, h, g_memdc, 0, 0, SRCCOPY);
}
/* ------------------------------------------------------------------ 编辑窗口 */

enum {
    ID_ED_EDIT = 300, ID_ED_ADD = 301, ID_ED_LIST = 302,
    ID_ED_TOGGLE = 303, ID_ED_DEL = 304, ID_ED_CLOSE = 305, ID_ED_TITLE = 306,
    ID_ED_HINT = 307,
    ID_BG_SECTION = 310, ID_BG_PATH = 311, ID_BG_PICK = 312, ID_BG_CLEAR = 313
};

static void ed_refresh_list(void) {
    if (!g_ed_list) return;
    SendMessageW(g_ed_list, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < g_ntodos; i++) {
        LRESULT idx = SendMessageW(g_ed_list, LB_ADDSTRING, 0, (LPARAM)g_todos[i].text);
        SendMessageW(g_ed_list, LB_SETITEMDATA, (WPARAM)idx, (LPARAM)g_todos[i].done);
    }
    if (g_ntodos > 0) {
        if (g_todo_sel >= g_ntodos) g_todo_sel = g_ntodos - 1;
        SendMessageW(g_ed_list, LB_SETCURSEL, (WPARAM)g_todo_sel, 0);
        /* 选中项如果在第一屏里，就把视图拉回顶部，别让首行被裁掉 */
        int item_h = 32;
        int vis = (g_ed_list_rect.bottom - g_ed_list_rect.top) / item_h;
        if (vis < 1) vis = 1;
        int top = 0;
        if (g_todo_sel >= vis) top = g_todo_sel - vis + 1;
        SendMessageW(g_ed_list, LB_SETTOPINDEX, (WPARAM)top, 0);
    }
    InvalidateRect(g_ed_list, NULL, TRUE);
}

static void ed_commit(void) {
    wchar_t buf[TODO_TEXT] = { 0 };
    GetWindowTextW(g_ed_edit, buf, TODO_TEXT);
    wchar_t *s = buf;
    while (*s == L' ' || *s == L'\t') s++;
    int n = (int)wcslen(s);
    while (n > 0 && (s[n - 1] == L' ' || s[n - 1] == L'\t')) s[--n] = 0;
    if (!s[0]) return;

    if (g_ed_index >= 0 && g_ed_index < g_ntodos) todo_edit(g_ed_index, s);
    else {
        int idx = todo_add(s);
        if (idx >= 0) g_todo_sel = idx;
    }
    g_ed_index = -1;
    SetWindowTextW(g_ed_edit, L"");
    SetWindowTextW(g_ed_add, L"添加");
    int src = todo_save();
    ed_refresh_list();
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

static void ed_begin_edit_selected(void) {
    int sel = (int)SendMessageW(g_ed_list, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= g_ntodos) return;
    g_ed_index = sel;
    SetWindowTextW(g_ed_edit, g_todos[sel].text);
    SetWindowTextW(g_ed_add, L"保存");
    SetFocus(g_ed_edit);
    SendMessageW(g_ed_edit, EM_SETSEL, 0, -1);
}

static LRESULT CALLBACK EditorEditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                       UINT_PTR id, DWORD_PTR ref) {
    if (msg == WM_PAINT) {
        LRESULT r = DefSubclassProc(h, msg, wp, lp);
        if (GetWindowTextLengthW(h) == 0) {
            HDC dc = GetDC(h);
            if (dc) {
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(92, 102, 124));
                HFONT oldf = (HFONT)SelectObject(dc, g_ed_font);
                RECT rc;
                GetClientRect(h, &rc);
                rc.left += 6;
                DrawTextW(dc, L"添加一条待办，回车确认", -1, &rc,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                SelectObject(dc, oldf);
                ReleaseDC(h, dc);
            }
        }
        return r;
    }
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN) { ed_commit(); return 0; }
        if (wp == VK_ESCAPE) {
            g_ed_index = -1;
            SetWindowTextW(g_ed_edit, L"");
            SetWindowTextW(g_ed_add, L"添加");
            SetFocus(g_ed_list);
            return 0;
        }
    }
    return DefSubclassProc(h, msg, wp, lp);
}

/* 列表：去掉系统滚动条后自己处理滚轮 */
static WNDPROC g_list_old = NULL;

static LRESULT CALLBACK ed_list_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEWHEEL) {
        int delta = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        int top = (int)SendMessageW(h, LB_GETTOPINDEX, 0, 0);
        top -= delta * 2;
        if (top < 0) top = 0;
        SendMessageW(h, LB_SETTOPINDEX, (WPARAM)top, 0);
        if (g_editor) InvalidateRect(g_editor, NULL, TRUE);
        return 0;
    }
    return CallWindowProcW(g_list_old, h, msg, wp, lp);
}

/* ---- 编辑器的扁平按钮（与主画面同一套视觉） ---- */

typedef struct { HWND hwnd; WNDPROC old; int hover; } EBtn;
static EBtn g_ebtn[8];
static int g_nebtn = 0;

static EBtn *ebtn_find(HWND h) {
    for (int i = 0; i < g_nebtn; i++)
        if (g_ebtn[i].hwnd == h) return &g_ebtn[i];
    return NULL;
}

static LRESULT CALLBACK ebtn_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    EBtn *e = ebtn_find(h);
    if (!e) return DefWindowProcW(h, msg, wp, lp);
    if (msg == WM_MOUSEMOVE && !e->hover) {
        e->hover = 1;
        TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, h, 0 };
        TrackMouseEvent(&t);
        InvalidateRect(h, NULL, TRUE);
    } else if (msg == WM_MOUSELEAVE && e->hover) {
        e->hover = 0;
        InvalidateRect(h, NULL, TRUE);
    }
    return CallWindowProcW(e->old, h, msg, wp, lp);
}

static void ebtn_track(HWND h) {
    if (!h || g_nebtn >= (int)(sizeof g_ebtn / sizeof g_ebtn[0])) return;
    g_ebtn[g_nebtn].hwnd = h;
    g_ebtn[g_nebtn].old = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)ebtn_proc);
    g_ebtn[g_nebtn].hover = 0;
    g_nebtn++;
}

static unsigned shade(unsigned rgb, double f) {
    int r = (int)(((rgb >> 16) & 0xFF) * f);
    int g = (int)(((rgb >> 8) & 0xFF) * f);
    int b = (int)((rgb & 0xFF) * f);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
}

static void ed_draw_button(const DRAWITEMSTRUCT *dis) {
    int primary = (dis->CtlID == ID_ED_ADD);
    int pressed = (dis->itemState & ODS_SELECTED) != 0;
    int disabled = (dis->itemState & ODS_DISABLED) != 0;
    EBtn *e = ebtn_find(dis->hwndItem);
    int hover = e && e->hover && !disabled;

    unsigned fill, border, txt;
    if (primary) {
        fill = g_pal.ink;
        border = g_pal.ink;
        txt = (hover || pressed) ? g_pal.onSignal : g_pal.paper;
        if (hover || pressed) fill = g_pal.signal;
    } else {
        fill = pressed ? g_pal.chip : g_pal.panel;
        border = hover ? g_pal.ink : 0x8A8A86;
        txt = disabled ? 0x9A9A96 : g_pal.ink;
    }

    RECT rc = dis->rcItem;
    GpGraphics *g = NULL;
    GdipCreateFromHDC(dis->hDC, &g);
    if (!g) return;
    GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
    GdipSetTextRenderingHint(g, TextRenderingHintClearTypeGridFit);

    float w = (float)(rc.right - rc.left), h = (float)(rc.bottom - rc.top);
    box_fill(g, g_pal.paper, 255, (float)rc.left, (float)rc.top, w, h);
    float cut = 7.0f;
    fill_cut(g, fill, 255, (float)rc.left + 1, (float)rc.top + 1, w - 2, h - 2, cut, 2);
    stroke_cut(g, border, 200, (float)rc.left + 1, (float)rc.top + 1, w - 2, h - 2, cut, 2, 1);

    wchar_t label[64] = { 0 };
    GetWindowTextW(dis->hwndItem, label, 64);
    GpFont *f = font_get(14, FONT_UI);
    int dy = pressed ? 1 : 0;
    text_cy(g, label, f, txt, 255, (float)rc.left, (float)rc.top + h * 0.5f + dy, w, 1, 14);

    GdipDeleteGraphics(g);
}

static void ed_list_draw(const DRAWITEMSTRUCT *dis) {
    if (dis->itemID == (UINT)-1) {
        FillRect(dis->hDC, &dis->rcItem, g_ed_edit_brush);
        return;
    }
    int done = (int)dis->itemData;
    int selected = (dis->itemState & ODS_SELECTED) != 0;

    RECT rc = dis->rcItem;
    HBRUSH bg = CreateSolidBrush(selected ? RGB(0xE4, 0xE4, 0xE0) : RGB(0xFF, 0xFF, 0xFF));
    FillRect(dis->hDC, &rc, bg);
    DeleteObject(bg);

    /* 选中：左侧 trace 竖条 */
    if (selected) {
        RECT bar = { rc.left, rc.top + 2, rc.left + 3, rc.bottom - 2 };
        HBRUSH bb = CreateSolidBrush(g_dark ? RGB(0xFF, 0xFA, 0x00) : RGB(0x16, 0x16, 0x16));
        FillRect(dis->hDC, &bar, bb);
        DeleteObject(bb);
    }

    int box = 14, pad = 12;
    int by = rc.top + (rc.bottom - rc.top - box) / 2;
    RECT cb = { rc.left + pad, by, rc.left + pad + box, by + box };

    GpGraphics *g = NULL;
    GdipCreateFromHDC(dis->hDC, &g);
    if (g) {
        GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
        Box bb = { (float)cb.left, (float)cb.top, (float)box, (float)box };
        draw_check(g, bb, done, 0, 1.0f);
        GdipDeleteGraphics(g);
    }

    wchar_t text[TODO_TEXT + 8] = { 0 };
    SendMessageW(g_ed_list, LB_GETTEXT, dis->itemID, (LPARAM)text);

    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, done ? RGB(0x5C, 0x5C, 0x5C) : RGB(0x16, 0x16, 0x16));
    HFONT old = (HFONT)SelectObject(dis->hDC, g_ed_font);
    RECT tr = rc;
    tr.left = cb.right + 12;
    DrawTextW(dis->hDC, text, -1, &tr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dis->hDC, old);

    if (done) {
        /* 完成：划线 */
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(0x5C, 0x5C, 0x5C));
        HGDIOBJ op = SelectObject(dis->hDC, pen);
        SIZE sz = { 0, 0 };
        HFONT of = (HFONT)SelectObject(dis->hDC, g_ed_font);
        GetTextExtentPoint32W(dis->hDC, text, (int)wcslen(text), &sz);
        SelectObject(dis->hDC, of);
        int ly = rc.top + (rc.bottom - rc.top) / 2;
        int lx = cb.right + 12;
        int maxx = rc.right - 12;
        if (lx + sz.cx > maxx) sz.cx = maxx - lx;
        MoveToEx(dis->hDC, lx, ly, NULL);
        LineTo(dis->hDC, lx + sz.cx, ly);
        SelectObject(dis->hDC, op);
        DeleteObject(pen);
    }

    /* 行分隔线 */
    HPEN line = CreatePen(PS_SOLID, 1, RGB(0xD9, 0xD9, 0xD6));
    HGDIOBJ ol = SelectObject(dis->hDC, line);
    MoveToEx(dis->hDC, rc.left + 10, rc.bottom - 1, NULL);
    LineTo(dis->hDC, rc.right - 10, rc.bottom - 1);
    SelectObject(dis->hDC, ol);
    DeleteObject(line);
}

static void ed_draw_scrollbar(GpGraphics *g);

static LRESULT CALLBACK EditorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, g_ed_bg_brush);

        GpGraphics *g = NULL;
        GdipCreateFromHDC(dc, &g);
        if (g) {
            GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
            /* 输入框的细边框（EDIT 本身无边框，居内 2px） */
            if (g_ed_field.right > g_ed_field.left) {
                float x = (float)g_ed_field.left + 0.5f, y = (float)g_ed_field.top + 0.5f;
                float w = (float)(g_ed_field.right - g_ed_field.left) - 1.0f;
                float h = (float)(g_ed_field.bottom - g_ed_field.top) - 1.0f;
                fill_round(g, argb(0xFFFFFF, 255), x, y, w, h, 0.0f);
                stroke_round(g, argb(0x161616, 200), x, y, w, h, 0.0f, 1.0f);
            }
            /* 列表区边框 + 细滚动指示 */
            if (g_ed_list_rect.right > g_ed_list_rect.left) {
                float x = (float)g_ed_list_rect.left + 0.5f, y = (float)g_ed_list_rect.top + 0.5f;
                float w = (float)(g_ed_list_rect.right - g_ed_list_rect.left) - 1.0f;
                float h = (float)(g_ed_list_rect.bottom - g_ed_list_rect.top) - 1.0f;
                stroke_round(g, argb(0xD9D9D6, 255), x, y, w, h, 0.0f, 1.0f);
                ed_draw_scrollbar(g);
            }
            GdipDeleteGraphics(g);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetDlgCtrlID((HWND)lp) == ID_ED_HINT
                        ? RGB(0x5C, 0x5C, 0x5C) : RGB(0x16, 0x16, 0x16));
        return (LRESULT)g_ed_bg_brush;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)wp;
        SetBkColor(dc, RGB(0xFF, 0xFF, 0xFF));
        SetTextColor(dc, RGB(0x16, 0x16, 0x16));
        return (LRESULT)g_ed_edit_brush;
    }

    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lp;
        if (dis->CtlID == ID_ED_LIST) ed_list_draw(dis);
        else ed_draw_button(dis);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_ED_ADD:
            if (HIWORD(wp) == BN_CLICKED) ed_commit();
            return 0;
        case ID_ED_TOGGLE: {
            int sel = (int)SendMessageW(g_ed_list, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < g_ntodos) {
                todo_toggle(sel);
                todo_save();
                ed_refresh_list();
                if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
            }
            return 0;
        }
        case ID_ED_DEL: {
            int sel = (int)SendMessageW(g_ed_list, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < g_ntodos) {
                todo_delete(sel);
                todo_save();
                ed_refresh_list();
                if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
            }
            return 0;
        }
        case ID_BG_PICK: {
            wchar_t path[MAX_PATH] = { 0 };
            if (pick_image(path, MAX_PATH)) {
                bg_apply(path);
                SetWindowTextW(g_bg_path_lbl, g_bg_image[0] ? g_bg_image : L"未设置（用内置渐变）");
            }
            return 0;
        }
        case ID_BG_CLEAR:
            bg_apply(NULL);
            SetWindowTextW(g_bg_path_lbl, L"未设置（用内置渐变）");
            return 0;
        case ID_ED_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            if (g_hwnd) SetForegroundWindow(g_hwnd);
            return 0;
        case ID_ED_LIST:
            if (HIWORD(wp) == LBN_DBLCLK) {
                int sel = (int)SendMessageW(g_ed_list, LB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < g_ntodos) {
                    todo_toggle(sel);
                    todo_save();
                    ed_refresh_list();
                    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
                }
            } else if (HIWORD(wp) == LBN_SELCHANGE) {
                int sel = (int)SendMessageW(g_ed_list, LB_GETCURSEL, 0, 0);
                if (sel >= 0) g_todo_sel = sel;
            }
            return 0;
        }
        return 0;

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        if (g_hwnd) SetForegroundWindow(g_hwnd);
        return 0;

    case WM_DESTROY:
        g_editor = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* 列表右侧的细滚动指示（系统滚动条已去掉，保持暗色统一） */
static void ed_draw_scrollbar(GpGraphics *g) {
    if (!g_ed_list || g_ntodos <= 0) return;
    int item_h = 32;
    int vis = (g_ed_list_rect.bottom - g_ed_list_rect.top) / item_h;
    if (vis <= 0 || g_ntodos <= vis) return;

    int top = (int)SendMessageW(g_ed_list, LB_GETTOPINDEX, 0, 0);
    float track_x = (float)g_ed_list_rect.right - 7.0f;
    float track_y = (float)g_ed_list_rect.top + 6.0f;
    float track_h = (float)(g_ed_list_rect.bottom - g_ed_list_rect.top) - 12.0f;
    float thumb_h = track_h * (float)vis / (float)g_ntodos;
    if (thumb_h < 18.0f) thumb_h = 18.0f;
    float max_top = (float)(g_ntodos - vis);
    float ratio = max_top > 0 ? (float)top / max_top : 0.0f;
    if (ratio < 0) ratio = 0;
    if (ratio > 1) ratio = 1;
    float thumb_y = track_y + (track_h - thumb_h) * ratio;

    fill_round(g, argb(0xE4E4E0, 255), track_x, track_y, 3.0f, track_h, 1.5f);
    fill_round(g, argb(0x9A9A96, 255), track_x, thumb_y, 3.0f, thumb_h, 1.5f);
}

static void ed_layout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    int pad = 18, row = 34, btn_h = 36;
    int k_bg = 118;      /* 底部背景图片区 */

    MoveWindow(g_ed_title, pad, pad - 4, w - pad * 2, 26, TRUE);

    /* 输入框：视觉边框比控件大 4px，EDIT 居中内缩 */
    int fx = pad, fy = pad + 32;
    int fw = w - pad * 2 - 96;
    g_ed_field.left = fx;
    g_ed_field.top = fy;
    g_ed_field.right = fx + fw;
    g_ed_field.bottom = fy + row;
    MoveWindow(g_ed_edit, fx + 2, fy + 2, fw - 4, row - 4, TRUE);
    MoveWindow(g_ed_add, fx + fw + 8, fy, 88, row, TRUE);

    int listy = fy + row + 14;
    int avail = h - listy - btn_h - pad - 42 - k_bg;
    int item_h = 32;
    int listh = (avail / item_h) * item_h;      /* 整行，避免半行被裁 */
    if (listh < item_h) listh = item_h;
    g_ed_list_rect.left = pad;
    g_ed_list_rect.top = listy;
    g_ed_list_rect.right = w - pad;
    g_ed_list_rect.bottom = listy + listh;
    MoveWindow(g_ed_list, pad, listy, w - pad * 2 - 12, listh, TRUE);

    int by = listy + listh + 14;
    MoveWindow(g_ed_toggle, pad, by, 112, btn_h, TRUE);
    MoveWindow(g_ed_delete, pad + 120, by, 88, btn_h, TRUE);
    MoveWindow(g_ed_close, w - pad - 88, by, 88, btn_h, TRUE);
    MoveWindow(g_ed_hint, pad, by + btn_h + 8, w - pad * 2, 22, TRUE);

    /* 背景图片区 */
    int bs = by + btn_h + 8 + 30;
    MoveWindow(g_bg_section, pad, bs, w - pad * 2, 22, TRUE);
    MoveWindow(g_bg_path_lbl, pad, bs + 24, w - pad * 2, 22, TRUE);
    MoveWindow(g_bg_pick, pad, bs + 52, 120, 32, TRUE);
    MoveWindow(g_bg_clear, pad + 128, bs + 52, 84, 32, TRUE);

    InvalidateRect(hwnd, NULL, TRUE);
}

/* 暗色标题栏 + 暗色滚动条，尽量和主画面统一 */
static void editor_dark_chrome(void) {
    /* 浅色主题：不去套暗色标题栏，只让滚动条别太突兀 */
    if (0) {
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm) {
        typedef HRESULT(WINAPI * SetAttr)(HWND, DWORD, LPCVOID, DWORD);
        SetAttr set_attr = (SetAttr)(void *)GetProcAddress(dwm, "DwmSetWindowAttribute");
        if (set_attr) {
            BOOL dark = TRUE;
            if (FAILED(set_attr(g_editor, 20, &dark, sizeof dark)))   /* Win10 2004+ */
                set_attr(g_editor, 19, &dark, sizeof dark);           /* 1809+ */
        }
        FreeLibrary(dwm);
    }
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    if (ux) {
        typedef HRESULT(WINAPI * SetTheme)(HWND, LPCWSTR, LPCWSTR);
        SetTheme set_theme = (SetTheme)(void *)GetProcAddress(ux, "SetWindowTheme");
        if (set_theme) {
            set_theme(g_ed_list, L"Explorer", NULL);
            set_theme(g_ed_edit, L"CFD", NULL);
        }
        FreeLibrary(ux);
    }
    }
}

static void show_editor(void) {
    if (g_editor && IsWindowVisible(g_editor)) {
        SetForegroundWindow(g_editor);
        SetFocus(g_ed_edit);
        return;
    }
    if (!g_ed_font) {
        g_ed_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_SWISS, g_face_ui);
        g_ed_font_title = CreateFontW(-19, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH | FF_SWISS, g_face_ui);
    }
    if (!g_ed_bg_brush) g_ed_bg_brush = CreateSolidBrush(RGB(0xF4, 0xF4, 0xF2));
    if (!g_ed_edit_brush) g_ed_edit_brush = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));

    HINSTANCE hinst = GetModuleHandleW(NULL);
    if (!g_ed_registered) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = EditorProc;
        wc.hInstance = hinst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = L"PomodoroTodoEditor";
        wc.hbrBackground = NULL;
        RegisterClassExW(&wc);
        g_ed_registered = 1;
    }

    if (!g_editor) {
        int W = 600, H = 640;
        g_editor = CreateWindowExW(WS_EX_TOPMOST, L"PomodoroTodoEditor", L"设置",
                                   WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                                   CW_USEDEFAULT, CW_USEDEFAULT, W, H,
                                   NULL, NULL, hinst, NULL);
        if (!g_editor) return;

        g_ed_title = CreateWindowExW(0, L"STATIC", L"待办清单",
                                     WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10,
                                     g_editor, (HMENU)(INT_PTR)ID_ED_TITLE, hinst, NULL);
        g_ed_edit = CreateWindowExW(0, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                    0, 0, 10, 10, g_editor,
                                    (HMENU)(INT_PTR)ID_ED_EDIT, hinst, NULL);
        g_ed_add = CreateWindowExW(0, L"BUTTON", L"添加",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                   0, 0, 10, 10, g_editor,
                                   (HMENU)(INT_PTR)ID_ED_ADD, hinst, NULL);
        g_ed_list = CreateWindowExW(0, L"LISTBOX", L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                    LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                                    0, 0, 10, 10, g_editor,
                                    (HMENU)(INT_PTR)ID_ED_LIST, hinst, NULL);
        g_ed_toggle = CreateWindowExW(0, L"BUTTON", L"切换完成",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                      0, 0, 10, 10, g_editor,
                                      (HMENU)(INT_PTR)ID_ED_TOGGLE, hinst, NULL);
        g_ed_delete = CreateWindowExW(0, L"BUTTON", L"删除",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                      0, 0, 10, 10, g_editor,
                                      (HMENU)(INT_PTR)ID_ED_DEL, hinst, NULL);
        g_ed_close = CreateWindowExW(0, L"BUTTON", L"关闭",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                     0, 0, 10, 10, g_editor,
                                     (HMENU)(INT_PTR)ID_ED_CLOSE, hinst, NULL);
        g_bg_section = CreateWindowExW(0, L"STATIC", L"背景图片", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                       0, 0, 10, 10, g_editor,
                                       (HMENU)(INT_PTR)ID_BG_SECTION, hinst, NULL);
        g_bg_path_lbl = CreateWindowExW(0, L"STATIC", L"未设置（用内置渐变）",
                                        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
                                        0, 0, 10, 10, g_editor,
                                        (HMENU)(INT_PTR)ID_BG_PATH, hinst, NULL);
        g_bg_pick = CreateWindowExW(0, L"BUTTON", L"选择图片…",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                    0, 0, 10, 10, g_editor,
                                    (HMENU)(INT_PTR)ID_BG_PICK, hinst, NULL);
        g_bg_clear = CreateWindowExW(0, L"BUTTON", L"清除",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                     0, 0, 10, 10, g_editor,
                                     (HMENU)(INT_PTR)ID_BG_CLEAR, hinst, NULL);
        g_ed_hint = CreateWindowExW(0, L"STATIC", L"回车添加  ·  双击切换完成  ·  选中后 Del 删除",
                                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                                    0, 0, 10, 10, g_editor,
                                    (HMENU)(INT_PTR)ID_ED_HINT, hinst, NULL);

        HWND all[] = { g_ed_edit, g_ed_add, g_ed_list, g_ed_toggle, g_ed_delete,
                       g_ed_close, g_ed_hint, g_bg_section, g_bg_path_lbl,
                       g_bg_pick, g_bg_clear };
        for (int i = 0; i < (int)(sizeof all / sizeof all[0]); i++)
            SendMessageW(all[i], WM_SETFONT, (WPARAM)g_ed_font, TRUE);
        SendMessageW(g_ed_title, WM_SETFONT, (WPARAM)g_ed_font_title, TRUE);

        SendMessageW(g_ed_list, LB_SETITEMHEIGHT, 0, (LPARAM)32);
        SetWindowSubclass(g_ed_edit, EditorEditProc, 1, 0);

        g_list_old = (WNDPROC)SetWindowLongPtrW(g_ed_list, GWLP_WNDPROC,
                                                (LONG_PTR)ed_list_proc);
        ebtn_track(g_ed_add);
        ebtn_track(g_ed_toggle);
        ebtn_track(g_ed_delete);
        ebtn_track(g_ed_close);
        ebtn_track(g_bg_pick);
        ebtn_track(g_bg_clear);
        editor_dark_chrome();
        ed_refresh_list();
    } else {
        ed_refresh_list();
    }

    if (g_bg_path_lbl)
        SetWindowTextW(g_bg_path_lbl, g_bg_image[0] ? g_bg_image : L"未设置（用内置渐变）");

    ShowWindow(g_editor, SW_SHOW);
    ed_layout(g_editor);
    SetForegroundWindow(g_editor);
    SetFocus(g_ed_edit);
}

/* ------------------------------------------------------------------ 主窗口 */

static wchar_t g_title_shown[256];

static void update_title(void) {
    wchar_t remain[16];
    fmt_seconds(g_pomo.remaining, remain, 16);
    swprintf(g_title, 256, L"%s %s · %s · 完成 %d · 待办 %d",
             phase_name_w(g_pomo.phase), remain,
             g_pomo.running ? L"运行中" : L"已暂停",
             g_pomo.completed, g_ntodos - todo_done_count());
    /* 16ms 定时器每秒调用它 60 次，只有内容真变了才 SetWindowText */
    if (wcscmp(g_title, g_title_shown) != 0) {
        wcsncpy(g_title_shown, g_title, 255);
        g_title_shown[255] = 0;
        SetWindowTextW(g_hwnd, g_title);
    }
}

static void apply_opacity(void) {
    BYTE a = kOpacity[g_opacity_index % (int)(sizeof kOpacity / sizeof kOpacity[0])];

    /* 分层窗口会让系统禁用 ClearType，小字就发虚。
       所以只有在真的需要透明时才挂 WS_EX_LAYERED。 */
    LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
    int want_layered = (a < 255);
    int is_layered = (ex & WS_EX_LAYERED) != 0;

    if (want_layered && !is_layered) {
        SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        SetLayeredWindowAttributes(g_hwnd, 0, a, LWA_ALPHA);
    } else if (!want_layered && is_layered) {
        SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
        SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    } else if (want_layered) {
        SetLayeredWindowAttributes(g_hwnd, 0, a, LWA_ALPHA);
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void toggle_fullscreen(void) {
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    if (g_fullscreen) {
        g_fullscreen = 0;
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW);
        int w = 980, h = 640;
        int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
        int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, x, y, w, h,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        g_fullscreen = 1;
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_POPUP);
        SetWindowPos(g_hwnd, HWND_TOPMOST, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        SetTimer(hwnd, 1, 16, NULL);
        return 0;

    case WM_TIMER: {
        ULONGLONG now = GetTickCount64();
        double dt = (now - g_last_tick) / 1000.0;
        g_last_tick = now;
        if (dt > 0.5) dt = 0.5;
        if (pomo_tick(&g_pomo, dt)) chime_play();

        double target = 0;
        if (g_list_pinned) target = 1;
        else if (g_list_every > 0) {
            g_list_clock += dt;
            double t = fmod(g_list_clock, (double)g_list_every);
            target = (t < g_list_dur) ? 1 : 0;
        }
        double speed = dt / 0.45;
        double prev_alpha = g_list_alpha;
        if (g_list_alpha < target) g_list_alpha = fmin(target, g_list_alpha + speed);
        else if (g_list_alpha > target) g_list_alpha = fmax(target, g_list_alpha - speed);
        int animating = (g_list_alpha != prev_alpha);

        /* 按钮悬停：黄色从左擦入 */
        {
            double step = dt * 14.0;
            if (step > 1.0) step = 1.0;
            for (int i = 0; i < UI_COUNT; i++) {
                double want = (g_ui_hover == i) ? 1.0 : 0.0;
                if (g_btn_hover_t[i] < want) {
                    g_btn_hover_t[i] += step;
                    if (g_btn_hover_t[i] > want) g_btn_hover_t[i] = want;
                    animating = 1;
                } else if (g_btn_hover_t[i] > want) {
                    g_btn_hover_t[i] -= step;
                    if (g_btn_hover_t[i] < want) g_btn_hover_t[i] = want;
                    animating = 1;
                }
            }
        }

        if (g_breath_period > 1.0)
            g_breath_phase = fmod(g_breath_phase + dt * 6.283185307179586 / g_breath_period,
                                  6.283185307179586);

        update_title();

        /* 按需重绘：淡入淡出期间 60fps（16ms 定时器），
           其余时间只在秒数或呼吸档位变化时刷一次，静止时几乎不耗 CPU。 */
        {
            static int last_sec = -1, last_bucket = -1;
            int sec_now = (int)time(NULL);
            int bucket = ((int)((0.5 + 0.5 * sin(g_breath_phase)) * 255.0)) & ~7;
            int need = animating;
            int bucket_changed = 0;

            if (sec_now != last_sec) { last_sec = sec_now; need = 1; }
            if (bucket != last_bucket) { last_bucket = bucket; need = 1; bucket_changed = 1; }

            if (need) {
                HWND fg = GetForegroundWindow();
                int active = (fg == hwnd) || (g_editor && IsWindowVisible(g_editor) &&
                                              (fg == g_editor || IsChild(g_editor, fg)));
                /* 后台时连呼吸都不刷，只保留秒针 */
                if (!active && !animating && bucket_changed) return 0;
                /* 不在前台时把瞬时动画降到 15fps，别白烧 CPU */
                static int skips = 0;
                if (animating && !active) {
                    if (++skips % 4 != 0) return 0;
                } else {
                    skips = 0;
                }
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        paint(dc, rc.right - rc.left, rc.bottom - rc.top);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_KEYDOWN:
        switch (wp) {
        case VK_ESCAPE: DestroyWindow(hwnd); return 0;
        case VK_SPACE:
            g_pomo.running = !g_pomo.running;
            update_title();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case 'R': case VK_F4: pomo_reset(&g_pomo); update_title();
                  InvalidateRect(hwnd, NULL, FALSE); return 0;
        case 'N': case VK_F5: pomo_skip(&g_pomo); update_title();
                  InvalidateRect(hwnd, NULL, FALSE); return 0;
        case 'T': case VK_F6: g_opacity_index++; apply_opacity(); return 0;
        case 'D': case VK_F7:
            palette_set(!g_dark);
            settings_save();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case VK_F11: toggle_fullscreen(); return 0;
        case 'L': list_toggle_pin();
                  InvalidateRect(hwnd, NULL, FALSE); return 0;

        case 'E':
        case VK_F2: show_editor(); return 0;
        case VK_F3: list_toggle_pin();
                    InvalidateRect(hwnd, NULL, FALSE); return 0;
        case VK_UP:
            if (g_ntodos) {
                g_todo_sel = (g_todo_sel + g_ntodos - 1) % g_ntodos;
                g_list_pinned = 1;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        case VK_DOWN:
            if (g_ntodos) {
                g_todo_sel = (g_todo_sel + 1) % g_ntodos;
                g_list_pinned = 1;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        return 0;

    case WM_MOUSEMOVE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
        int hit = ui_hit(mx, my);
        int row = hit_row(mx, my, rc.right, rc.bottom);
        int dia = hit_dial(mx, my, rc.right, rc.bottom);
        if (hit != g_ui_hover || row != g_hover_row) {
            g_ui_hover = hit;
            g_hover_row = row;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        if (!g_ui_tracking) {
            TRACKMOUSEEVENT tme = { sizeof tme, TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            g_ui_tracking = 1;
        }
        SetCursor(LoadCursorW(NULL, (hit >= 0 || row >= 0 || dia) ? IDC_HAND : IDC_ARROW));
        return 0;
    }

    case WM_MOUSELEAVE:
        g_ui_tracking = 0;
        if (g_ui_hover != -1 || g_hover_row != -1) {
            g_ui_hover = -1;
            g_hover_row = -1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (g_ntodos > 0) {
            if (delta < 0) { if (g_scroll < g_ntodos - 1) g_scroll++; }
            else { if (g_scroll > 0) g_scroll--; }
            g_list_pinned = 1;
            g_list_alpha = 1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
        int hit = ui_hit(mx, my);
        int was = g_ui_press;
        g_ui_press = -1;
        ReleaseCapture();
        if (was >= 0 && was == hit) {
            switch (was) {
            case UI_RUN:   g_pomo.running = !g_pomo.running; break;
            case UI_RESET: pomo_reset(&g_pomo); break;
            case UI_SKIP:  pomo_skip(&g_pomo); break;
            case UI_LIST:  list_toggle_pin(); break;
            case UI_EDIT:  show_editor(); break;
            case UI_QUIT:  DestroyWindow(hwnd); return 0;
            }
            update_title();
        }
        if (g_row_press >= 0) {
            int row = hit_row(mx, my, rc.right, rc.bottom);
            if (row == g_row_press && row >= 0) {
                todo_toggle(row);
                todo_save();
                g_todo_sel = row;
            }
            g_row_press = -1;
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);

        int hit = ui_hit(mx, my);
        if (hit >= 0) {
            g_ui_press = hit;
            SetCapture(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        int row = hit_row(mx, my, rc.right, rc.bottom);
        if (row >= 0) {
            g_row_press = row;
            g_todo_sel = row;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        if (hit_dial(mx, my, rc.right, rc.bottom)) {
            g_pomo.running = !g_pomo.running;
            update_title();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        return 0;
    }

    case WM_SETCURSOR: {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        RECT rc;
        GetClientRect(hwnd, &rc);
        int hand = (ui_hit(pt.x, pt.y) >= 0) || hit_row(pt.x, pt.y, rc.right, rc.bottom) >= 0 ||
                   hit_dial(pt.x, pt.y, rc.right, rc.bottom);
        SetCursor(LoadCursorW(NULL, hand ? IDC_HAND : IDC_ARROW));
        return TRUE;
    }

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        if (g_editor) DestroyWindow(g_editor);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ 自测 */

static int g_test_fail = 0;

static void check(int cond, const char *what) {
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) g_test_fail++;
}

static int selftest(void) {
    printf("番茄钟 + 待办清单 自测\n");
    printf("----------------\n");

    Pomodoro p;
    pomo_init(&p);
    check(p.phase == PHASE_WORK, "初始阶段是专注");
    check(p.remaining == 25 * 60, "专注默认 25 分钟");
    pomo_tick(&p, 10);
    check(p.remaining == 25 * 60, "暂停时推进不扣时间");
    p.running = 1;
    pomo_tick(&p, 60);
    check(fabs(p.remaining - 24 * 60) < 1e-6, "运行 60 秒后剩 24:00");
    pomo_tick(&p, 24 * 60);
    check(p.phase == PHASE_SHORT && p.remaining == 300, "专注结束进短休 5 分钟");
    pomo_tick(&p, 300);
    check(p.phase == PHASE_WORK, "短休结束回专注");
    for (int i = 0; i < 3; i++) { pomo_tick(&p, 1500); if (i < 2) pomo_tick(&p, 300); }
    check(p.completed == 4 && p.phase == PHASE_LONG, "第 4 个专注后长休");
    p.phase = PHASE_WORK; p.remaining = 1; p.running = 1;
    check(pomo_tick(&p, 100) == 1 && p.remaining == 300 - 99, "超时时间结转到下一阶段");
    p.running = 0;
    double before = p.remaining;
    pomo_tick(&p, 30);
    check(p.remaining == before, "暂停后不扣时间");
    pomo_reset(&p);
    check(p.remaining == 1500 && p.phase == PHASE_WORK, "重置回初始态");

    printf("-- 待办清单 --\n");
    g_ntodos = 0;
    g_todo_sel = 0;
    check(todo_add(L"写周报") == 0, "新增第 1 条");
    check(todo_add(L"复现 bug") == 1, "新增第 2 条");
    check(todo_add(L"看书") == 2, "新增第 3 条");
    check(g_ntodos == 3, "共 3 条");
    check(todo_add(L"") == -1, "拒绝新增空条目");

    todo_toggle(1);
    check(g_todos[1].done == 1 && todo_done_count() == 1, "标记完成后计数为 1");
    todo_toggle(1);
    check(g_todos[1].done == 0 && todo_done_count() == 0, "取消完成");

    todo_edit(0, L"写季度周报");
    check(wcscmp(g_todos[0].text, L"写季度周报") == 0, "编辑第 1 条");
    check(todo_edit(9, L"x") == -1, "越界编辑被拒绝");

    todo_delete(1);
    check(g_ntodos == 2 && wcscmp(g_todos[1].text, L"看书") == 0, "删除第 2 条后顺序正确");
    todo_toggle(1);
    check(todo_done_count() == 1, "再次标记完成");

    wchar_t tmpdir[MAX_PATH], tmpfile[MAX_PATH];
    GetModuleFileNameW(NULL, tmpdir, MAX_PATH);
    { wchar_t *s = wcsrchr(tmpdir, L'\\'); if (s) *s = 0; }
    swprintf(tmpfile, MAX_PATH, L"%s\\mp3rn_todo_test.txt", tmpdir);
    DeleteFileW(tmpfile);
    wcscpy(g_todo_path, tmpfile);
    int save_rc = todo_save();
    printf("    (todo_save 返回 %d, 路径 %ls)\n", save_rc, tmpfile);
    check(save_rc == 0 && GetFileAttributesW(tmpfile) != INVALID_FILE_ATTRIBUTES,
          "清单写盘成功");

    Todo saved[2];
    int saved_n = g_ntodos;
    memcpy(saved, g_todos, sizeof(Todo) * (size_t)saved_n);

    g_ntodos = 0;
    todo_load();
    check(g_ntodos == saved_n, "读盘条目数一致");
    check(wcscmp(g_todos[0].text, saved[0].text) == 0, "读回第 1 条文本一致");
    check(g_todos[1].done == saved[1].done, "读回完成状态一致");
    DeleteFileW(tmpfile);
    g_todo_path[0] = 0;

    printf("----------------\n");
    printf("%s（%d 项失败）\n", g_test_fail ? "失败" : "全部通过", g_test_fail);
    return g_test_fail ? 1 : 0;
}

/* ------------------------------------------------------------------ 入口 */

/* 激活 ComCtl32 v6：占位提示、暗色滚动条等都需要它。
   没有资源文件，就把 manifest 写到临时目录再激活。 */
static void enable_visual_styles(void) {
    static const char *manifest =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\n"
        "  <assemblyIdentity type=\"win32\" name=\"mp3rn.pomodoro\" version=\"1.0.0.0\""
        " processorArchitecture=\"*\"/>\n"
        "  <dependency><dependentAssembly>\n"
        "    <assemblyIdentity type=\"win32\" name=\"Microsoft.Windows.Common-Controls\""
        " version=\"6.0.0.0\" processorArchitecture=\"*\""
        " publicKeyToken=\"6595b64144ccf1df\" language=\"*\"/>\n"
        "  </dependentAssembly></dependency>\n"
        "</assembly>\n";

    wchar_t tmpdir[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmpdir)) return;
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%smp3rn_pomodoro_manifest.xml", tmpdir);
    path[MAX_PATH - 1] = 0;

    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(f, manifest, (DWORD)strlen(manifest), &wrote, NULL);
    CloseHandle(f);

    ACTCTXW ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cbSize = sizeof ctx;
    ctx.lpSource = path;
    HANDLE act = CreateActCtxW(&ctx);
    if (act != INVALID_HANDLE_VALUE) {
        ULONG_PTR cookie = 0;
        ActivateActCtx(act, &cookie);
    }
}

int main(int argc, char **argv) {
    enable_visual_styles();
    SetConsoleOutputCP(CP_UTF8);
    pomo_init(&g_pomo);

    palette_set(0);
    int want_selftest = 0, want_help = 0;
    wchar_t render_path[MAX_PATH] = { 0 };
    int render_w = 1920, render_h = 1080;
    int theme_override = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dark")) theme_override = 1;
        if (!strcmp(argv[i], "--theme") && i + 1 < argc) {
            theme_override = strcmp(argv[++i], "dark") ? 0 : 1;
        }
        if (!strcmp(argv[i], "--render") && i + 1 < argc) {
            MultiByteToWideChar(CP_UTF8, 0, argv[++i], -1, render_path, MAX_PATH);
        }
        if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            int ww = 0, hh = 0;
            if (sscanf(argv[++i], "%dx%d", &ww, &hh) == 2 && ww > 0 && hh > 0) {
                render_w = ww; render_h = hh;
            }
        }
        if (!strcmp(argv[i], "--selftest")) want_selftest = 1;
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) want_help = 1;
        if (!strcmp(argv[i], "--window")) g_fullscreen = 0;
        if (!strcmp(argv[i], "--mute")) g_mute = 1;
        if (!strcmp(argv[i], "--bg-image") && i + 1 < argc) {
            int n = MultiByteToWideChar(CP_UTF8, 0, argv[++i], -1, NULL, 0);
            if (n > 0) {
                wchar_t *w2 = malloc(sizeof(wchar_t) * (size_t)n);
                if (w2) {
                    MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, w2, n);
                    wcsncpy(g_bg_image, w2, MAX_PATH - 1);
                    free(w2);
                }
            }
        }
        if (!strcmp(argv[i], "--breath-phase") && i + 1 < argc)
            g_breath_phase = atof(argv[++i]);
        if (!strcmp(argv[i], "--breath-period") && i + 1 < argc) {
            double v = atof(argv[++i]);
            if (v > 2.0) g_breath_period = v;
        }
        if (!strcmp(argv[i], "--font-check")) {
            GdiplusStartupInput gi2;
            memset(&gi2, 0, sizeof gi2);
            gi2.GdiplusVersion = 1;
            ULONG_PTR tok = 0;
            if (GdiplusStartup(&tok, &gi2, NULL) != Ok) return 1;
            resolve_families();

    if (g_bg_image[0]) {
        if (!bg_image_load(g_bg_image)) {
            g_bg_image[0] = 0;      /* 图片失效就退回内置渐变 */
        }
    }
            {
                char a[256] = { 0 }, b[256] = { 0 };
                WideCharToMultiByte(CP_UTF8, 0,
                                    g_face_ui_used[0] ? g_face_ui_used : L"(none)",
                                    -1, a, sizeof a, NULL, NULL);
                WideCharToMultiByte(CP_UTF8, 0,
                                    g_face_display_used[0] ? g_face_display_used : L"(none)",
                                    -1, b, sizeof b, NULL, NULL);
                printf("text font : %s\n", a);
                printf("display   : %s%s\n", b, g_display_needs_bold ? " + Bold" : "");
            }
            GdiplusShutdown(tok);
            return 0;
        }
        if (!strcmp(argv[i], "--test-chime")) {
            chime_build();
            if (!g_chime) { printf("合成失败\n"); return 1; }
            short *s = (short *)((unsigned char *)g_chime + 44);
            int n = (g_chime_len - 44) / 2;
            int peak = 0;
            double sum = 0;
            for (int k = 0; k < n; k++) {
                int a = s[k] < 0 ? -s[k] : s[k];
                if (a > peak) peak = a;
                sum += (double)s[k] * s[k];
            }
            printf("提示音: %d ms, %d Hz, 峰值 %.0f%% , RMS %.0f%%\n",
                   (int)(n * 1000 / 22050), 22050,
                   peak * 100.0 / 32768.0, sqrt(sum / n) * 100.0 / 32768.0);
            PlaySoundW((LPCWSTR)g_chime, NULL, SND_MEMORY | SND_SYNC | SND_NODEFAULT);
            printf("已播放\n");
            return 0;
        }
        if (!strcmp(argv[i], "--opacity") && i + 1 < argc) g_opacity_index = atoi(argv[++i]);
        if (!strcmp(argv[i], "--list-every") && i + 1 < argc) g_list_every = atoi(argv[++i]);
        if (!strcmp(argv[i], "--list-dur") && i + 1 < argc) g_list_dur = atoi(argv[++i]);
        if ((!strcmp(argv[i], "--work") || !strcmp(argv[i], "--short") ||
             !strcmp(argv[i], "--long")) && i + 1 < argc) {
            char *flag = argv[i];
            int v = atoi(argv[++i]);
            if (v > 0 && v <= 600) {
                if (!strcmp(flag, "--work"))  g_pomo.work_min = v;
                if (!strcmp(flag, "--short")) g_pomo.short_min = v;
                if (!strcmp(flag, "--long"))  g_pomo.long_min = v;
            }
        }
        if (!strcmp(argv[i], "--todo-file") && i + 1 < argc) {
            int n = MultiByteToWideChar(CP_UTF8, 0, argv[++i], -1, NULL, 0);
            if (n > 0) {
                wchar_t *w = malloc(sizeof(wchar_t) * (size_t)n);
                if (w) {
                    MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, w, n);
                    wcsncpy(g_todo_path, w, MAX_PATH - 1);
                    g_todo_path[MAX_PATH - 1] = 0;
                    free(w);
                }
            }
        }
    }

    if (want_selftest) return selftest();

    if (want_help) {
        printf("用法: 桌面时钟番茄钟 [选项]\n"
               "  --work N          专注分钟（默认 25）\n"
               "  --short N         短休分钟（默认 5）\n"
               "  --long N          长休分钟（默认 15）\n"
               "  --list-every N    每隔 N 秒浮出待办（默认 30，0=不自动浮出）\n"
               "  --list-dur N      每次浮出 M 秒（默认 8）\n"
               "  --breath-period N 背景明暗循环周期，秒（默认 26）\n"
               "  --todo-file P     指定待办清单文件\n"
               "  --window          窗口模式启动\n"
               "  --opacity N       启动时的透明度档位（0=不透明）\n"
               "  --mute            不响提示音\n"
               "  --test-chime      试听提示音后退出\n"
               "  --font-check      打印实际使用的字体\n"
               "  --theme light|dark 启动主题（默认浅色）\n"
               "  --render P --size WxH  把界面渲染成 PNG 后退出（版式自检）\n"
               "  --selftest        只跑 26 项自测，不开窗口\n"
               "按键: 空格 开始/暂停 · R/F4 重置 · N/F5 跳过 · L/F3 待办 · E/F2 编辑\n"
               "      T/F6 透明度 · D/F7 深浅 · F11 窗口/全屏 · Esc 退出\n");
        return 0;
    }

    g_pomo.total = g_pomo.remaining = g_pomo.work_min * 60.0;
    settings_path_init();
    settings_load();
    if (theme_override >= 0) palette_set(theme_override);
    if (!g_todo_path[0]) todo_path_init();
    todo_load();

    GdiplusStartupInput gi;
    memset(&gi, 0, sizeof gi);
    gi.GdiplusVersion = 1;
    ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &gi, NULL) != Ok) {
        MessageBoxW(NULL, L"GDI+ 初始化失败", L"错误", MB_ICONERROR);
        return 1;
    }

    resolve_families();

    if (theme_override >= 0) palette_set(theme_override);

    if (g_bg_image[0]) {
        if (!bg_image_load(g_bg_image)) {
            g_bg_image[0] = 0;      /* 图片失效就退回内置渐变 */
        }
    }

    if (render_path[0]) {
        g_list_pinned = 1;
        g_list_alpha = 1;
        int ok = render_png(render_path, render_w, render_h);
        printf("%s %dx%d -> %ls\n", ok ? "已渲染" : "渲染失败", render_w, render_h, render_path);
        GdiplusShutdown(token);
        return ok ? 0 : 1;
    }

    SetProcessDPIAware();

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"PomodoroClock";
    wc.hbrBackground = NULL;
    HICON ico = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                  0, 0, LR_DEFAULTSIZE | LR_SHARED);
    wc.hIcon = ico ? ico : LoadIconW(NULL, IDI_APPLICATION);
    wc.hIconSm = ico;
    if (!RegisterClassExW(&wc)) {
        MessageBoxW(NULL, L"注册窗口类失败", L"错误", MB_ICONERROR);
        return 1;
    }

    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoW(MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
    int sw = mi.rcMonitor.right - mi.rcMonitor.left;
    int sh = mi.rcMonitor.bottom - mi.rcMonitor.top;

    DWORD style = g_fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    int wx = mi.rcMonitor.left, wy = mi.rcMonitor.top, ww = sw, wh = sh;
    if (!g_fullscreen) {
        ww = 980; wh = 640;
        wx = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - ww) / 2;
        wy = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - wh) / 2;
    }

    DWORD ex_style = WS_EX_TOPMOST;
    if (g_opacity_index % 4 != 0) ex_style |= WS_EX_LAYERED;
    HWND hwnd = CreateWindowExW(ex_style, L"PomodoroClock",
                                L"桌面时钟番茄钟", style, wx, wy, ww, wh,
                                NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) {
        MessageBoxW(NULL, L"创建窗口失败", L"错误", MB_ICONERROR);
        return 1;
    }
    g_hwnd = hwnd;

    /* 主窗口不需要打字：把 IME 上下文摘掉。
       否则中文输入法下按 E/R/N/L/T 会被当成拼音输入，
       弹输入法候选框，按键永远传不到这里。 */
    ImmAssociateContext(hwnd, NULL);

    apply_opacity();
    update_title();
    g_last_tick = GetTickCount64();
    /* 启动时先不显示清单，等第一个轮播周期再露 */
    g_list_clock = (double)g_list_dur;

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    InvalidateRect(hwnd, NULL, FALSE);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    GdiplusShutdown(token);
    return 0;
}
