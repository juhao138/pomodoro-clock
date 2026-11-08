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
#include <imm.h>
#include <gdiplus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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
#define FONT_CACHE 24
static FontSlot g_fonts[FONT_CACHE];
static int g_nfonts = 0;

#define FONT_DISPLAY 0
#define FONT_UI      1

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
static int g_display_needs_bold = 0;
static wchar_t g_face_ui[LF_FACESIZE] = L"Microsoft YaHei UI";
static wchar_t g_face_display[LF_FACESIZE] = L"Segoe UI Semibold";
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

    GpFontFamily *family = (kind == FONT_DISPLAY) ? g_family_display : g_family_ui;
    if (!family) family = g_family_ui;
    INT style = FontStyleRegular;
    if (kind == FONT_DISPLAY && g_display_needs_bold) style = FontStyleBold;

    GpFont *font = NULL;
    GdipCreateFont(family, (REAL)size_px, style, UnitPixel, &font);
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
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
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

    RectF full = { 0, 0, (REAL)w, (REAL)h };
    GpLineGradient *lg = NULL;
    GdipCreateLineBrushFromRect(&full, (ARGB)0xFF141A30, (ARGB)0xFF090D18,
                                LinearGradientModeForwardDiagonal, WrapModeTileFlipXY, &lg);
    if (lg) {
        GdipFillRectangle(g, (GpBrush *)lg, 0, 0, (REAL)w, (REAL)h);
        GdipDeleteBrush((GpBrush *)lg);
    }

    glow(g, w * 0.06f, h * 0.70f, w * 0.40f, 0xFF6B5A, (int)(11 * glow_scale));
    glow(g, w * 0.97f, h * 0.00f, w * 0.46f, 0x5B7BFF, (int)(15 * glow_scale));
    glow(g, w * 0.58f, h * 0.96f, w * 0.38f, 0x2FD3A0, (int)(7 * glow_scale));
    glow(g, w * 0.00f, h * 0.00f, w * 0.30f, 0x8E6BFF, (int)(9 * glow_scale));

    GdipDeleteGraphics(g);
}

/* 合成结果缓存：alpha 量化到 8 的倍数，只有跨档时才重新 AlphaBlend */
static HDC g_bgc;
static HBITMAP g_bgcbmp;
static int g_bgc_w, g_bgc_h;
static int g_bgc_alpha = -1;

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
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
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
    BitBlt(g_bgc, 0, 0, w, h, g_bgdc[0], 0, 0, SRCCOPY);
    BLENDFUNCTION bf;
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = (BYTE)alpha;
    bf.AlphaFormat = 0;
    AlphaBlend(g_bgc, 0, 0, w, h, g_bgdc[1], 0, 0, w, h, bf);
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

typedef struct { float x, y, w, h; } Box;

/* ---------------- Google 风两栏：时钟靠左，待办靠右 ---------------- */

static float kk(int h)         { return (float)(h / 1080.0); }
static float margin_l(int w)   { return (float)(w * 0.072); }
static float todo_x0(int w)    { return (float)(w * 0.615); }
static float todo_x1(int w)    { return (float)(w * 0.925); }
static float list_top_y(int h) { return (float)(h * 0.225); }
static float list_row_h(int h) { return kk(h) * 50.0f; }

static int list_visible_rows(int h) {
    float avail = (float)(h * 0.80) - list_top_y(h);
    int rows = (int)(avail / list_row_h(h));
    return rows < 1 ? 1 : rows;
}

/* 左：时钟 + 番茄钟，全部贴左边距 */
static void paint_clock_block(GpGraphics *g, int w, int h) {
    float k = kk(h);
    float x0 = margin_l(w);
    RectF full = { 0, 0, (REAL)w, (REAL)h };

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t clock[32];
    swprintf(clock, 32, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);

    /* 时钟：左对齐，右边界不能压到待办栏 */
    double cy = h * 0.243;
    double size = h * 0.17;
    double max_w = todo_x0(w) - x0 - w * 0.03;
    GpFont *f = font_get((int)size, FONT_DISPLAY);
    RectF bounds;
    for (int guard = 0; guard < 24 && f; guard++) {
        INT cw = 0, ch = 0;
        GdipMeasureString(g, clock, -1, f, &full, NULL, &bounds, &cw, &ch);
        if (bounds.Width <= max_w) break;
        size *= 0.94;
        f = font_get((int)size, FONT_DISPLAY);
    }
    text_draw_a(g, clock, f, C_TEXT, 255, x0,
                (float)(cy - bounds.Height * 0.5),
                (float)(w - x0 * 2), (float)bounds.Height, 0);

    /* 日期 */
    static const wchar_t *wd[] = { L"星期日", L"星期一", L"星期二", L"星期三",
                                   L"星期四", L"星期五", L"星期六" };
    wchar_t datestr[128];
    swprintf(datestr, 128, L"%d 年 %d 月 %d 日   %s",
             st.wYear, st.wMonth, st.wDay, wd[st.wDayOfWeek]);
    GpFont *f_date = font_get((int)(k * 24), FONT_UI);
    text_draw_a(g, datestr, f_date, C_MUTED, 255, x0, (float)(h * 0.362),
                (float)(w - x0 * 2), (float)(k * 40), 0);

    /* 番茄钟：环在左，时间在环右侧 */
    double radius = k * 78.0;
    double thick = fmax(3.0, k * 12);
    double ring_cy = h * 0.612;
    double ring_cx = x0 + radius;
    unsigned accent = phase_color(g_pomo.phase);

    arc_draw(g, (float)ring_cx, (float)ring_cy, (float)radius, (float)thick,
             0, 360, argb(C_TRACK, 255), 0);

    double frac = g_pomo.total > 0 ? g_pomo.remaining / g_pomo.total : 0;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    if (frac > 0.0008) {
        arc_draw(g, (float)ring_cx, (float)ring_cy, (float)radius, (float)thick,
                 -90, (float)(360.0 * frac), argb(accent, 255), 1);
        double rad = (-90 + 360.0 * frac) * 3.14159265358979 / 180.0;
        dot_draw(g, (float)(ring_cx + cos(rad) * radius),
                 (float)(ring_cy + sin(rad) * radius),
                 (float)(thick * 0.55), argb(accent, 255));
    }

    /* 环内：阶段名 */
    GpFont *f_phase = font_get((int)(k * 16), FONT_UI);
    text_draw_a(g, phase_name_w(g_pomo.phase), f_phase, accent, 255,
                (float)(ring_cx - radius), (float)(ring_cy - k * 10),
                (float)(radius * 2), (float)(k * 22), 1);

    /* 环右：剩余时间（大）+ 状态（小） */
    float tx = (float)(ring_cx + radius + k * 40);
    wchar_t remain[16];
    fmt_seconds(g_pomo.remaining, remain, 16);
    GpFont *f_remain = font_get((int)(k * 74), FONT_DISPLAY);
    text_draw_a(g, remain, f_remain, C_TEXT, 255, tx,
                (float)(ring_cy - k * 50), (float)(w - tx), (float)(k * 74), 0);

    GpFont *f_state = font_get((int)(k * 15), FONT_UI);
    wchar_t state[64];
    swprintf(state, 64, L"%s", g_pomo.running ? L"进行中" : L"已暂停");
    text_draw_a(g, state, f_state, g_pomo.running ? accent : C_DIM, 225, tx,
                (float)(ring_cy + k * 26), (float)(w - tx), (float)(k * 22), 0);

    /* 完成点：环下方 */
    int group = g_pomo.completed % g_pomo.long_every;
    if (g_pomo.phase == PHASE_WORK && g_pomo.completed > 0 && group == 0)
        group = g_pomo.long_every;
    double dot_r = fmax(2.5, k * 4.5);
    double gap = dot_r * 4.2;
    double dy = ring_cy + radius + k * 34;
    for (int i = 0; i < g_pomo.long_every; i++) {
        double x = ring_cx - gap * (g_pomo.long_every - 1) / 2.0 + i * gap;
        dot_draw(g, (float)x, (float)dy, (float)dot_r,
                 i < group ? argb(accent, 255) : argb(C_TRACK, 255));
    }
}

/* 右：毛玻璃待办卡片 */
typedef struct {
    float x0, y0, x1, y1;
    float pad, header, row_h;
    int rows;
} Card;

static Card todo_card(int w, int h) {
    float k = kk(h);
    Card c;
    c.pad = k * 26;
    c.header = k * 40;
    c.row_h = k * 46;
    c.x1 = (float)(w * 0.945);
    c.x0 = c.x1 - (float)(w * 0.345);
    int maxr = (int)((h * 0.44) / c.row_h);
    if (maxr < 1) maxr = 1;
    int rows = g_ntodos > 0 ? g_ntodos : 1;
    if (rows > maxr) rows = maxr;
    c.rows = rows;
    float ch = c.pad * 2 + c.header + rows * c.row_h;
    c.y0 = (float)(h * 0.42) - ch / 2;
    c.y1 = c.y0 + ch;
    return c;
}

static void paint_todo_card(GpGraphics *g, int w, int h, int alpha) {
    float k = kk(h);
    Card c = todo_card(w, h);
    float cw = c.x1 - c.x0;
    float chh = c.y1 - c.y0;
    float r = k * 20;

    if (alpha <= 2) return;

    /* 投影 */
    GpPath *sp = round_rect_path(c.x0, c.y0 + k * 10, cw, chh, r);
    if (sp) {
        GpSolidFill *sb = brush_new(argb(0x05070E, (int)(alpha * 0.28)));
        if (sb) {
            GdipFillPath(g, (GpBrush *)sb, sp);
            GdipDeleteBrush((GpBrush *)sb);
        }
        GdipDeletePath(sp);
    }

    /* 玻璃本体：圆角裁剪 + 白色竖向渐变 */
    GpPath *pp = round_rect_path(c.x0, c.y0, cw, chh, r);
    if (!pp) return;
    if (GdipSetClipPath(g, pp, CombineModeReplace) == Ok) {
        RectF rb = { c.x0, c.y0, cw, chh };
        GpLineGradient *lg = NULL;
        GdipCreateLineBrushFromRect(&rb,
                                    (ARGB)argb(0xFFFFFF, (int)(alpha * 0.14)),
                                    (ARGB)argb(0xFFFFFF, (int)(alpha * 0.04)),
                                    LinearGradientModeVertical, WrapModeTileFlipXY, &lg);
        if (lg) {
            GdipFillRectangle(g, (GpBrush *)lg, c.x0, c.y0, cw, chh);
            GdipDeleteBrush((GpBrush *)lg);
        }
        GdipResetClip(g);
    }
    GdipDeletePath(pp);

    /* 边框 + 顶部亮线 */
    stroke_round(g, argb(0xFFFFFF, (int)(alpha * 0.22)), c.x0, c.y0, cw, chh, r, 1.0f);
    fill_round(g, argb(0xFFFFFF, (int)(alpha * 0.18)),
               c.x0 + k * 22, c.y0 + 1.0f, cw - k * 44, 1.0f, 0.5f);

    /* ---- 内容 ---- */
    GpFont *f_head = font_get((int)(k * 15), FONT_UI);
    GpFont *f_item = font_get((int)(k * 19), FONT_UI);
    float pad = c.pad;
    float hx = c.x0 + pad;
    float hw = cw - pad * 2;
    float hy = c.y0 + pad * 0.72f;

    text_draw_a(g, L"待办清单", f_head, 0xEAF0FA, (int)(alpha * 0.85),
                hx, hy, hw * 0.7f, k * 24, 0);
    wchar_t cnt[32];
    swprintf(cnt, 32, L"%d / %d", todo_done_count(), g_ntodos);
    text_draw_a(g, cnt, f_head, 0xEAF0FA, (int)(alpha * 0.55),
                hx + hw * 0.7f, hy, hw * 0.3f, k * 24, 2);

    fill_round(g, argb(0xFFFFFF, (int)(alpha * 0.10)),
               hx, c.y0 + pad * 0.72f + k * 30, hw, (float)fmax(1.0, k * 1.0), 0.5f);

    if (g_ntodos == 0) {
        text_draw_a(g, L"按 E 添加待办", f_item, 0xEAF0FA, (int)(alpha * 0.5),
                    hx, c.y0 + pad + c.header, hw, c.row_h, 0);
        return;
    }

    int first = 0;
    if (g_todo_sel >= c.rows) first = g_todo_sel - c.rows + 1;
    float top = c.y0 + pad + c.header;

    for (int rr = 0; rr < c.rows; rr++) {
        int i = first + rr;
        if (i >= g_ntodos) break;
        float ry = top + rr * c.row_h;
        float cb = k * 16.0f;
        float cbx = hx;
        float cby = ry + (c.row_h - cb) / 2.0f;

        if (i == g_todo_sel && g_list_pinned) {
            fill_round(g, argb(0xFFFFFF, (int)(alpha * 0.10)),
                       hx - k * 10, ry + k * 2, hw + k * 20, c.row_h - k * 4, k * 9);
        }

        if (g_todos[i].done) {
            fill_round(g, argb(C_BREAK, alpha), cbx, cby, cb, cb, k * 4);
            GpPen *pen = NULL;
            GdipCreatePen1((ARGB)argb(0x0B111C, alpha), (REAL)fmax(1.6, k * 2.0),
                           UnitPixel, &pen);
            if (pen) {
                GdipDrawLine(g, pen, cbx + cb * 0.24f, cby + cb * 0.52f,
                             cbx + cb * 0.44f, cby + cb * 0.72f);
                GdipDrawLine(g, pen, cbx + cb * 0.44f, cby + cb * 0.72f,
                             cbx + cb * 0.78f, cby + cb * 0.28f);
                GdipDeletePen(pen);
            }
        } else {
            stroke_round(g, argb(0xFFFFFF, (int)(alpha * 0.42)), cbx, cby, cb, cb, k * 4,
                         (float)fmax(1.2, k * 1.3));
        }

        float tx = cbx + cb + k * 13;
        float tw = hx + hw - tx;
        unsigned color = g_todos[i].done ? 0x8C97AC : 0xF4F7FC;
        text_draw_a(g, g_todos[i].text, f_item, color, alpha, tx, ry, tw, c.row_h, 0);

        if (g_todos[i].done) {
            INT mw = 0, mh = 0;
            RectF lay = { tx, ry, tw, c.row_h };
            RectF got;
            GdipMeasureString(g, g_todos[i].text, -1, f_item, &lay, NULL, &got, &mw, &mh);
            GpPen *pen = NULL;
            GdipCreatePen1((ARGB)argb(0x8C97AC, (int)(alpha * 0.8)), (REAL)fmax(1.0, k * 1.1),
                           UnitPixel, &pen);
            if (pen) {
                float ly = got.Y + got.Height * 0.52f;
                GdipDrawLine(g, pen, got.X, ly, got.X + got.Width, ly);
                GdipDeletePen(pen);
            }
        }
    }

    if (g_ntodos > c.rows) {
        GpFont *f_more = font_get((int)(k * 13), FONT_UI);
        wchar_t more[64];
        swprintf(more, 64, L"还有 %d 条 · E 打开编辑", g_ntodos - c.rows);
        text_draw_a(g, more, f_more, 0xEAF0FA, (int)(alpha * 0.45),
                    hx, c.y1 - pad * 0.55f, hw, k * 18, 0);
    }
}

static void paint(HDC target, int w, int h) {
    buffer_size(w, h);
    GpGraphics *g = g_gp;
    if (!g) return;

    float k = kk(h);
    if (k < 0.4f) k = 0.4f;
    if (k > 3.0f) k = 3.0f;

    render_background(w, h);

    /* 呼吸：暗相 → 亮相交叉淡入。t 在 0..1 之间按正弦摆动，
       亮的是极光本身（光晕 45% ↔ 145%），不是盖一层灰。 */
    {
        double t01 = 0.5 + 0.5 * sin(g_breath_phase);
        int alpha = ((int)(t01 * 255.0)) & ~7;      /* 量化，减少重合成 */
        composite_bg(w, h, alpha);
        if (g_bgc) BitBlt(g_memdc, 0, 0, w, h, g_bgc, 0, 0, SRCCOPY);
    }

    RectF full = { 0, 0, (REAL)w, (REAL)h };

    /* 左上角竖向强调条：替代原来的小圆点，给左上角一个视觉锚点，
       同时和时钟/日期的左边缘对齐 */
    {
        float bar_x = margin_l(w);
        float bar_w = (float)fmax(3.0, k * 4.0);
        float bar_h = k * 30.0f;
        fill_round(g, argb(phase_color(g_pomo.phase), 235),
                   bar_x, (float)(h * 0.118), bar_w, bar_h, bar_w * 0.5f);
    }

    int a = (int)(g_list_alpha * 255);
    if (a > 255) a = 255;
    if (a < 0) a = 0;

    paint_clock_block(g, w, h);
    paint_todo_card(g, w, h, a);

    /* 底部提示：贴左下，和时钟对齐 */
    GpFont *f_hint = font_get((int)(k * 15), FONT_UI);
    const wchar_t *hint = g_pomo.running
        ? L"空格 暂停  ·  R/F4 重置  ·  N/F5 跳过  ·  L/F3 待办  ·  E/F2 编辑  ·  T/F6 透明度  ·  F11 窗口  ·  Esc 退出"
        : L"空格 开始  ·  R/F4 重置  ·  N/F5 跳过  ·  L/F3 待办  ·  E/F2 编辑  ·  T/F6 透明度  ·  F11 窗口  ·  Esc 退出";
    text_draw_a(g, hint, f_hint, C_MUTED, 200, margin_l(w), (float)(h - k * 62),
                (float)(w - margin_l(w) * 2), (float)(k * 26), 0);

    BitBlt(target, 0, 0, w, h, g_memdc, 0, 0, SRCCOPY);
}

/* ------------------------------------------------------------------ 编辑窗口 */

enum {
    ID_ED_EDIT = 300, ID_ED_ADD = 301, ID_ED_LIST = 302,
    ID_ED_TOGGLE = 303, ID_ED_DEL = 304, ID_ED_CLOSE = 305, ID_ED_TITLE = 306,
    ID_ED_HINT = 307
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

    unsigned accent = phase_color(g_pomo.phase);
    unsigned fill, border, txt;
    if (primary) {
        fill = disabled ? 0x3B4256 : pressed ? shade(accent, 0.82)
             : hover ? shade(accent, 1.12) : accent;
        border = fill;
        txt = disabled ? 0x7A849A : 0x0B111C;
        if (hover || pressed) txt = 0xFFFFFF;
        txt = disabled ? 0x7A849A : 0xFFFFFF;
    } else {
        fill = disabled ? 0x141B2A : pressed ? 0x27324A
             : hover ? 0x1E2840 : 0x18202F;
        border = disabled ? 0x232B3D : (hover ? shade(accent, 0.9) : 0x2C3752);
        txt = disabled ? 0x5A6478 : 0xF4F7FC;
    }

    RECT rc = dis->rcItem;
    GpGraphics *g = NULL;
    GdipCreateFromHDC(dis->hDC, &g);
    if (!g) return;
    GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
    GdipSetTextRenderingHint(g, TextRenderingHintClearTypeGridFit);

    /* 先铺和窗口一致的底色，圆角外才不会露出白边 */
    fill_round(g, argb(0x121826, 255), (float)rc.left, (float)rc.top,
               (float)(rc.right - rc.left), (float)(rc.bottom - rc.top), 0);
    float bx = (float)rc.left + 0.5f, by = (float)rc.top + 0.5f;
    float bw = (float)(rc.right - rc.left) - 1.0f, bh = (float)(rc.bottom - rc.top) - 1.0f;
    fill_round(g, argb(fill, 255), bx, by, bw, bh, 8.0f);
    stroke_round(g, argb(border, 255), bx, by, bw, bh, 8.0f, 1.0f);

    wchar_t label[64] = { 0 };
    GetWindowTextW(dis->hwndItem, label, 64);
    GpFont *f = font_get(15, FONT_UI);
    int dy = pressed ? 1 : 0;
    text_draw_a(g, label, f, txt, 255, (float)rc.left,
                (float)(rc.top + dy), (float)(rc.right - rc.left),
                (float)(rc.bottom - rc.top), 1);

    GdipDeleteGraphics(g);
}

static void ed_list_draw(const DRAWITEMSTRUCT *dis) {
    if (dis->itemID == (UINT)-1) {
        FillRect(dis->hDC, &dis->rcItem, g_ed_edit_brush);
        return;
    }
    int done = (int)dis->itemData;
    int selected = (dis->itemState & ODS_SELECTED) != 0;

    HBRUSH bg = CreateSolidBrush(selected ? RGB(30, 41, 66) : RGB(22, 29, 45));
    FillRect(dis->hDC, &dis->rcItem, bg);
    DeleteObject(bg);

    RECT rc = dis->rcItem;
    int box = 15, pad = 10;
    int by = rc.top + (rc.bottom - rc.top - box) / 2;
    RECT cb = { rc.left + pad, by, rc.left + pad + box, by + box };

    HBRUSH fill = CreateSolidBrush(done ? RGB(53, 196, 143) : RGB(22, 29, 45));
    HPEN pen = CreatePen(PS_SOLID, 1, done ? RGB(53, 196, 143) : RGB(90, 100, 120));
    HGDIOBJ ob = SelectObject(dis->hDC, fill);
    HGDIOBJ op = SelectObject(dis->hDC, pen);
    RoundRect(dis->hDC, cb.left, cb.top, cb.right, cb.bottom, 6, 6);
    SelectObject(dis->hDC, ob);
    SelectObject(dis->hDC, op);
    DeleteObject(fill);
    DeleteObject(pen);

    if (done) {
        HPEN check = CreatePen(PS_SOLID, 2, RGB(11, 17, 28));
        HGDIOBJ oc = SelectObject(dis->hDC, check);
        MoveToEx(dis->hDC, cb.left + 3, cb.top + 7, NULL);
        LineTo(dis->hDC, cb.left + 6, cb.top + 10);
        LineTo(dis->hDC, cb.left + 11, cb.top + 4);
        SelectObject(dis->hDC, oc);
        DeleteObject(check);
    }

    wchar_t text[TODO_TEXT + 8] = { 0 };
    SendMessageW(g_ed_list, LB_GETTEXT, dis->itemID, (LPARAM)text);

    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, done ? RGB(120, 128, 145) : RGB(244, 247, 252));
    HFONT old = (HFONT)SelectObject(dis->hDC, g_ed_font);
    RECT tr = rc;
    tr.left = cb.right + 10;
    DrawTextW(dis->hDC, text, -1, &tr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dis->hDC, old);

    if (dis->itemState & ODS_FOCUS) {
        RECT f = rc;
        InflateRect(&f, -1, -1);
        DrawFocusRect(dis->hDC, &f);
    }
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
                fill_round(g, argb(0x161D2D, 255), x, y, w, h, 8.0f);
                stroke_round(g, argb(0x2C3752, 255), x, y, w, h, 8.0f, 1.0f);
            }
            /* 列表区边框 + 细滚动指示 */
            if (g_ed_list_rect.right > g_ed_list_rect.left) {
                float x = (float)g_ed_list_rect.left + 0.5f, y = (float)g_ed_list_rect.top + 0.5f;
                float w = (float)(g_ed_list_rect.right - g_ed_list_rect.left) - 1.0f;
                float h = (float)(g_ed_list_rect.bottom - g_ed_list_rect.top) - 1.0f;
                stroke_round(g, argb(0x2C3752, 255), x, y, w, h, 10.0f, 1.0f);
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
                        ? RGB(110, 120, 140) : RGB(244, 247, 252));
        return (LRESULT)g_ed_bg_brush;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)wp;
        SetBkColor(dc, RGB(22, 29, 45));
        SetTextColor(dc, RGB(244, 247, 252));
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

    fill_round(g, argb(0x1B2436, 255), track_x, track_y, 3.0f, track_h, 1.5f);
    fill_round(g, argb(0x3A4763, 255), track_x, thumb_y, 3.0f, thumb_h, 1.5f);
}

static void ed_layout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    int pad = 18, row = 34, btn_h = 36;

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
    int avail = h - listy - btn_h - pad - 42;
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

    InvalidateRect(hwnd, NULL, TRUE);
}

/* 暗色标题栏 + 暗色滚动条，尽量和主画面统一 */
static void editor_dark_chrome(void) {
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
            set_theme(g_ed_list, L"DarkMode_Explorer", NULL);
            set_theme(g_ed_edit, L"DarkMode_CFD", NULL);
        }
        FreeLibrary(ux);
    }
}

static void show_editor(void) {
    if (g_editor && IsWindowVisible(g_editor)) {
        SetForegroundWindow(g_editor);
        SetFocus(g_ed_edit);
        return;
    }
    if (!g_ed_font) {
        g_ed_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_SWISS, g_face_ui);
        g_ed_font_title = CreateFontW(-20, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH | FF_SWISS, g_face_display);
    }
    if (!g_ed_bg_brush) g_ed_bg_brush = CreateSolidBrush(RGB(18, 24, 38));
    if (!g_ed_edit_brush) g_ed_edit_brush = CreateSolidBrush(RGB(22, 29, 45));

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
        int W = 560, H = 520;
        g_editor = CreateWindowExW(WS_EX_TOPMOST, L"PomodoroTodoEditor", L"待办清单",
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
        g_ed_hint = CreateWindowExW(0, L"STATIC", L"回车添加  ·  双击切换完成  ·  选中后 Del 删除",
                                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                                    0, 0, 10, 10, g_editor,
                                    (HMENU)(INT_PTR)ID_ED_HINT, hinst, NULL);

        HWND all[] = { g_ed_edit, g_ed_add, g_ed_list,
                       g_ed_toggle, g_ed_delete, g_ed_close, g_ed_hint };
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
        editor_dark_chrome();
        ed_refresh_list();
    } else {
        ed_refresh_list();
    }

    ShowWindow(g_editor, SW_SHOW);
    ed_layout(g_editor);
    SetForegroundWindow(g_editor);
    SetFocus(g_ed_edit);
}

/* ------------------------------------------------------------------ 主窗口 */

static void update_title(void) {
    wchar_t remain[16];
    fmt_seconds(g_pomo.remaining, remain, 16);
    swprintf(g_title, 256, L"%s %s · %s · 完成 %d · 待办 %d",
             phase_name_w(g_pomo.phase), remain,
             g_pomo.running ? L"运行中" : L"已暂停",
             g_pomo.completed, g_ntodos - todo_done_count());
    SetWindowTextW(g_hwnd, g_title);
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
        SetTimer(hwnd, 1, 100, NULL);
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
        if (g_list_alpha < target) g_list_alpha = fmin(target, g_list_alpha + speed);
        else if (g_list_alpha > target) g_list_alpha = fmax(target, g_list_alpha - speed);

        if (g_breath_period > 1.0)
            g_breath_phase = fmod(g_breath_phase + dt * 6.283185307179586 / g_breath_period,
                                  6.283185307179586);

        update_title();

        /* 不在前台时没必要 10fps：降到约 2fps，省电省 CPU。
           自己就是前台、或者编辑窗口开着，才保持流畅刷新。 */
        {
            HWND fg = GetForegroundWindow();
            int active = (fg == hwnd) || (g_editor && IsWindowVisible(g_editor) &&
                                          (fg == g_editor || IsChild(g_editor, fg)));
            static int idle_tick = 0;
            if (active) {
                idle_tick = 0;
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (++idle_tick >= 5) {
                idle_tick = 0;
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
        case VK_F11: toggle_fullscreen(); return 0;
        case 'L': g_list_pinned = !g_list_pinned;
                  if (g_list_pinned) g_list_clock = 0;
                  InvalidateRect(hwnd, NULL, FALSE); return 0;

        case 'E':
        case VK_F2: show_editor(); return 0;
        case VK_F3: g_list_pinned = !g_list_pinned;
                    if (g_list_pinned) g_list_clock = 0;
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

    case WM_LBUTTONDOWN: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right, h = rc.bottom;
        float k = kk(h);
        int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);

        /* 点进度环：开始/暂停 */
        double radius = k * 78.0;
        double ring_cx = margin_l(w) + radius;
        double ring_cy = h * 0.612;
        if (hypot(mx - ring_cx, my - ring_cy) <= radius * 1.3) {
            g_pomo.running = !g_pomo.running;
            update_title();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        /* 点玻璃卡片里的某一行：切换完成 */
        if (g_list_alpha > 0.5 && g_ntodos > 0) {
            Card c = todo_card(w, h);
            float top = c.y0 + c.pad + c.header;
            int first = (g_todo_sel >= c.rows) ? g_todo_sel - c.rows + 1 : 0;
            if (mx >= c.x0 && mx <= c.x1 && my >= top && my < top + c.row_h * c.rows) {
                int i = first + (int)((my - top) / c.row_h);
                if (i >= 0 && i < g_ntodos) {
                    todo_toggle(i);
                    todo_save();
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
        }
        return 0;
    }

    case WM_SETCURSOR:
        SetCursor(LoadCursorW(NULL, IDC_ARROW));
        return TRUE;

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

    int want_selftest = 0, want_help = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest")) want_selftest = 1;
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) want_help = 1;
        if (!strcmp(argv[i], "--window")) g_fullscreen = 0;
        if (!strcmp(argv[i], "--mute")) g_mute = 1;
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
               "  --selftest        只跑 26 项自测，不开窗口\n"
               "按键: 空格 开始/暂停 · R/F4 重置 · N/F5 跳过 · L/F3 待办 · E/F2 编辑\n"
               "      T/F6 透明度 · F11 窗口/全屏 · Esc 退出\n");
        return 0;
    }

    g_pomo.total = g_pomo.remaining = g_pomo.work_min * 60.0;
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
