// Cheats: Action Replay DS codes from a usrcheat.dat (the R4 / TWiLight Menu++ database), chosen
// in a menu on the bottom screen and run once a frame.
//
// The codes address the DS's memory. The port's game data is laid out by GCC, elsewhere, so each
// absolute address goes through a table of DS -> port ranges made at build time (port/cheatmap.py:
// the decomp names much of its data after its DS address). Pointers the codes read out of the
// game are port pointers already and are used as they are. An address that doesn't translate is
// not written (and a test reading it fails): codes that patch the DS's machine code, or that write
// fixed heap addresses, do nothing here instead of corrupting memory.
//
// Files (sdmc:/3ds/soulsilver-3ds/): usrcheat.dat (or TWiLight's sd:/_nds/TWiLightMenu/extras/
// usrcheat.dat), cheats.txt (the names of the codes turned on). Menu: ZL+ZR, or L+R+SELECT; it
// also holds the display settings (settings.txt).
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port_ds.h"
#include "port.h"

#define PORT_DIR "sdmc:/3ds/soulsilver-3ds"

// ---------------------------------------------------------------- DS address -> port address
// (filled in after linking: u32 count, then count x { DS address, port address, length })
#define MAP_CAP 8192
// (volatile: filled in after linking, so the compiler mustn't fold its placeholder; non-zero: in the file)
__attribute__((used)) const volatile u32 port_cheat_map[1 + 3 * MAP_CAP] = { 0xC4EA7000 };
extern char __end__[];  // (the end of the program's own data)

static u8 *translate(u32 a, u32 len) {
    if (a >= 0x02000000 + PORT_DS_DELTA && a + len <= 0x02800000 + PORT_DS_DELTA) return (u8 *)a;  // (a port pointer into DS memory)
    if (a >= 0x00100000 && a + len <= (u32)__end__) return (u8 *)a;                            // (a port pointer into the game's data)
    if (a >= 0x04000000 && a + len <= 0x04800000) return (u8 *)(a + PORT_DS_DELTA);             // (I/O: the keys, ...)
    if (a < 0x02000000 || a >= 0x02400000) return NULL;
    u32 n = port_cheat_map[0];
    if (n > MAP_CAP) return NULL;
    const volatile u32 *t = port_cheat_map + 1;
    u32 lo = 0, hi = n;  // (the last range starting at or before a)
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (t[mid * 3] <= a) lo = mid + 1; else hi = mid;
    }
    if (!lo) return NULL;
    const volatile u32 *r = t + (lo - 1) * 3;
    if (a + len > r[0] + r[2]) return NULL;
    return (u8 *)(r[1] + (a - r[0]));
}

// ---------------------------------------------------------------- the codes
typedef struct {
    char name[72];
    u32 *words;
    u32 nwords;
    int folder;  // index into sFolders, -1: none
    u8 on;
} Cheat;
typedef struct {
    char name[72];
    u8 one;      // only one of its codes at a time
    u8 open;
} Folder;

static Cheat *sCheats;
static int sNumCheats;
static Folder *sFolders;
static int sNumFolders;
static char sDbName[64];

static u32 crc32(const u8 *p, u32 n) {
    u32 c = 0xFFFFFFFF;
    for (u32 i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320 & -(c & 1));
    }
    return c;  // (usrcheat.dat stores this, not inverted)
}

static void read_cstr(const u8 *p, u32 *at, u32 end, char *out, u32 cap) {
    u32 i = 0;
    while (*at < end && p[*at]) {
        if (i + 1 < cap) out[i++] = (char)p[*at];
        (*at)++;
    }
    out[i] = 0;
    if (*at < end) (*at)++;
}

// The game's codes from a usrcheat.dat: its entry by game ID and header CRC.
static int load_db(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    u8 head[0x200];
    port_rom_read(0, head, sizeof(head));
    u32 crc = crc32(head, 0x200);
    u8 magic[12];
    if (fread(magic, 1, 12, f) != 12 || memcmp(magic, "R4 CheatCode", 12)) { fclose(f); return 0; }
    fseek(f, 0x10, SEEK_SET);
    if (fread(sDbName, 1, sizeof(sDbName) - 1, f)) {}
    u32 off = 0, next = 0, entry[4];
    fseek(f, 0x100, SEEK_SET);
    while (fread(entry, 4, 4, f) == 4 && entry[2]) {
        if (!memcmp(&entry[0], head + 0x0C, 4) && entry[1] == crc) {
            off = entry[2];
            u32 e2[4];
            next = fread(e2, 4, 4, f) == 4 ? e2[2] : 0;
            break;
        }
    }
    if (!off) { fclose(f); port_log("cheats: %s has no entry for this ROM (CRC %08lX)\n", path, crc); return 0; }
    fseek(f, 0, SEEK_END);
    u32 fend = (u32)ftell(f);
    u32 size = (next > off ? next : fend) - off;
    u8 *d = malloc(size);
    fseek(f, off, SEEK_SET);
    if (!d || fread(d, 1, size, f) != size) { fclose(f); free(d); return 0; }
    fclose(f);
    char tmp[256];
    u32 at = 0;
    read_cstr(d, &at, size, tmp, sizeof(tmp));  // (the game's name)
    at = (at + 3) & ~3u;
    u32 count = (*(u32 *)(d + at)) & 0xFFFF;
    at += 4 + 32;  // (count, master codes)
    sCheats = calloc(count, sizeof(Cheat));
    sFolders = calloc(count, sizeof(Folder));
    int folderLeft = 0, folder = -1;
    for (u32 i = 0; i < count && at + 4 <= size; i++) {
        u32 h = *(u32 *)(d + at);
        u32 start = at + 4;
        at = start;
        char name[256], desc[256];
        read_cstr(d, &at, size, name, sizeof(name));
        read_cstr(d, &at, size, desc, sizeof(desc));
        at = (at + 3) & ~3u;
        if ((h >> 28) & 1) {  // a folder: the next (h & 0xFFFFFF) items are in it
            Folder *fo = &sFolders[sNumFolders];
            snprintf(fo->name, sizeof(fo->name), "%s", name);
            fo->one = (h >> 24) & 1;
            folder = sNumFolders++;
            folderLeft = (int)(h & 0xFFFFFF);
            continue;
        }
        u32 nw = *(u32 *)(d + at);
        Cheat *c = &sCheats[sNumCheats++];
        snprintf(c->name, sizeof(c->name), "%s", name);
        c->nwords = nw;
        c->words = malloc(nw * 4);
        if (c->words) memcpy(c->words, d + at + 4, nw * 4);
        c->folder = folderLeft > 0 ? folder : -1;
        if (folderLeft > 0) folderLeft--;
        at = start + 4 * (h & 0xFFFFFF);
    }
    free(d);
    port_log("cheats: %d codes in %d folders from %s\n", sNumCheats, sNumFolders, path);
    return 1;
}

// ---------------------------------------------------------------- display settings
extern int port_display_fill, port_side_fill, port_stereo_enabled, port_seam_fill;
static void save_settings(void) {
    FILE *f = fopen(PORT_DIR "/settings.txt", "w");
    if (!f) return;
    fprintf(f, "screen %s\nsides %s\n3d %s\nseams %s\n", port_display_fill ? "fill" : "1:1", port_side_fill ? "continue" : "black", port_stereo_enabled ? "on" : "off", port_seam_fill ? "filled" : "off");
    fclose(f);
}
static void load_settings(void) {
    FILE *f = fopen(PORT_DIR "/settings.txt", "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "screen fill", 11)) port_display_fill = 1;
        else if (!strncmp(line, "screen 1:1", 10)) port_display_fill = 0;
        else if (!strncmp(line, "sides continue", 14)) port_side_fill = 1;
        else if (!strncmp(line, "sides black", 11)) port_side_fill = 0;
        else if (!strncmp(line, "3d on", 5)) port_stereo_enabled = 1;
        else if (!strncmp(line, "3d off", 6)) port_stereo_enabled = 0;
        else if (!strncmp(line, "seams filled", 12)) port_seam_fill = 1;
        else if (!strncmp(line, "seams off", 9)) port_seam_fill = 0;
    }
    fclose(f);
}

static void save_choices(void) {
    FILE *f = fopen(PORT_DIR "/cheats.txt", "w");
    if (!f) return;
    for (int i = 0; i < sNumCheats; i++) {
        if (!sCheats[i].on) continue;
        const char *fn = sCheats[i].folder >= 0 ? sFolders[sCheats[i].folder].name : "";
        fprintf(f, "%s\t%s\n", fn, sCheats[i].name);
    }
    fclose(f);
}

static void load_choices(void) {
    FILE *f = fopen(PORT_DIR "/cheats.txt", "r");
    if (!f) return;
    char line[200];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        for (int i = 0; i < sNumCheats; i++) {
            const char *fn = sCheats[i].folder >= 0 ? sFolders[sCheats[i].folder].name : "";
            if (!strcmp(fn, line) && !strcmp(sCheats[i].name, tab + 1)) sCheats[i].on = 1;
        }
    }
    fclose(f);
}

void port_cheats_init(void) {
    load_settings();
    if (!load_db(PORT_DIR "/usrcheat.dat") && !load_db("sdmc:/_nds/TWiLightMenu/extras/usrcheat.dat")) return;
    load_choices();
}

// ---------------------------------------------------------------- the Action Replay DS engine
static int rd(u32 a, int size, u32 *v) {
    u8 *p = translate(a, (u32)size);
    if (!p) return 0;
    *v = size == 4 ? *(u32 *)p : size == 2 ? *(u16 *)p : *p;
    return 1;
}
int port_cheat_log;  // (debug.txt "cheatlog": each write, with the value it replaces)
static void wr(u32 a, int size, u32 v) {
    u8 *p = translate(a, (u32)size);
    if (port_cheat_log) {
        u32 old = !p ? 0 : size == 4 ? *(u32 *)p : size == 2 ? *(u16 *)p : *p;
        port_log("cheat write %08lX -> %p (%d bytes): %08lX -> %08lX%s\n", a, (void *)p, size, old, v, p ? "" : " (not translated: skipped)");
    }
    if (!p) return;
    if (size == 4) *(u32 *)p = v; else if (size == 2) *(u16 *)p = (u16)v; else *p = (u8)v;
}

static void run_code(const u32 *w, u32 n) {
    u32 offset = 0, data = 0;
    u32 cond = 0;       // nested conditions open
    u32 falseAt = 0;    // >0: inside a false condition opened at this depth
    u32 loopAt = 0, loopLeft = 0;
    int looping = 0;
    for (u32 i = 0; i + 1 < n; i += 2) {
        u32 a = w[i], b = w[i + 1];
        u32 t = a >> 28, x = a & 0x0FFFFFFF;
        if (t == 0xE) {  // (its data lines are skipped either way)
            u32 lines = (b + 7) / 8;
            if (!falseAt) for (u32 k = 0; k < b; k++) {
                u32 word = (i + 2 + k / 4 < n) ? w[i + 2 + k / 4] : 0;
                wr(x + offset + k, 1, (word >> ((k & 3) * 8)) & 0xFF);
            }
            i += lines * 2;
            continue;
        }
        if (t >= 0x3 && t <= 0xA) {  // conditions
            cond++;
            if (falseAt) continue;
            u32 addr = x ? x : offset, v;
            int ok;
            if (t <= 0x6) {
                ok = rd(addr, 4, &v);
                if (ok) ok = t == 3 ? b > v : t == 4 ? b < v : t == 5 ? b == v : b != v;
            } else {
                ok = rd(addr, 2, &v);
                u32 m = b >> 16, y = b & 0xFFFF;
                v &= ~m & 0xFFFF;
                if (ok) ok = t == 7 ? y > v : t == 8 ? y < v : t == 9 ? y == v : y != v;
            }
            if (!ok) falseAt = cond;
            continue;
        }
        if (a == 0xD0000000) {  // end if
            if (cond) { if (falseAt == cond) falseAt = 0; cond--; }
            continue;
        }
        if (a == 0xD1000000 || a == 0xD2000000) {  // next (loop), and for D2: end everything
            if (!falseAt && looping && loopLeft) { loopLeft--; i = loopAt - 2; continue; }
            looping = 0;
            if (a == 0xD2000000) { cond = 0; falseAt = 0; offset = 0; data = 0; }
            continue;
        }
        if (falseAt) continue;
        switch (t) {
        case 0x0: wr(x + offset, 4, b); break;
        case 0x1: wr(x + offset, 2, b); break;
        case 0x2: wr(x + offset, 1, b); break;
        case 0xB: { u32 v; offset = rd(x + offset, 4, &v) ? v : 0; break; }
        case 0xC:
            if ((a >> 24) == 0xC0) { looping = 1; loopAt = i + 2; loopLeft = b; }
            else if ((a >> 24) == 0xC6) wr(b, 4, offset);
            break;
        case 0xD:
            switch (a >> 24) {
            case 0xD3: offset = b; break;
            case 0xD4: data += b; break;
            case 0xD5: data = b; break;
            case 0xD6: wr(b + offset, 4, data); offset += 4; break;
            case 0xD7: wr(b + offset, 2, data); offset += 2; break;
            case 0xD8: wr(b + offset, 1, data); offset += 1; break;
            case 0xD9: { u32 v; data = rd(b + offset, 4, &v) ? v : 0; break; }
            case 0xDA: { u32 v; data = rd(b + offset, 2, &v) ? v : 0; break; }
            case 0xDB: { u32 v; data = rd(b + offset, 1, &v) ? v : 0; break; }
            case 0xDC: offset += b; break;
            }
            break;
        case 0xF:  // copy b bytes from [offset] to [x]
            for (u32 k = 0; k < b; k++) { u32 v; if (rd(offset + k, 1, &v)) wr(x + k, 1, v); }
            break;
        }
    }
}

static void run_cheats(void) {
    for (int i = 0; i < sNumCheats; i++)
        if (sCheats[i].on && sCheats[i].words) run_code(sCheats[i].words, sCheats[i].nwords);
}

// ---------------------------------------------------------------- the menu
// Rows: folders (A opens/closes) and the codes shown (A turns on/off). B or the menu keys: back.
typedef struct { int folder, cheat; } Row;
static Row *sRows;
static int sNumRows;

#define NUM_SETTINGS 4
static void build_rows(void) {
    if (!sRows) sRows = malloc(sizeof(Row) * (size_t)(sNumCheats + sNumFolders + NUM_SETTINGS + 1));
    sNumRows = 0;
    for (int i = 0; i < NUM_SETTINGS; i++) sRows[sNumRows++] = (Row){ -2, i };  // (the display settings)
    int lastFolder = -2;
    for (int i = 0; i < sNumCheats; i++) {
        int fo = sCheats[i].folder;
        if (fo != lastFolder && fo >= 0) sRows[sNumRows++] = (Row){ fo, -1 };
        lastFolder = fo;
        if (fo < 0 || sFolders[fo].open) sRows[sNumRows++] = (Row){ fo, i };
    }
}

static void draw_menu(int cur, int top) {
    printf("\x1b[1;1H\x1b[2J");
    printf("\x1b[36mSettings & cheats\x1b[0m  A: change  B: back\n");
    printf("\x1b[90m%.38s\x1b[0m\n", sNumCheats ? sDbName : "(no usrcheat.dat: no cheats)");
    for (int r = top; r < sNumRows && r < top + 26; r++) {
        const Row *w = &sRows[r];
        const char *mark = r == cur ? "\x1b[33m>" : " ";
        if (w->folder == -2) {
            if (w->cheat == 0) printf("%s Top screen: %s\x1b[0m\n", mark, port_display_fill ? "fill (1.25x, soft)" : "1:1 (exact pixels)");
            else if (w->cheat == 1) printf("%s Sides (fill): %s\x1b[0m\n", mark, port_side_fill ? "continue picture" : "black");
            else if (w->cheat == 2) printf("%s 3D (slider, 1:1): %s\x1b[0m\n", mark, port_stereo_enabled ? "on" : "off (faster)");
            else printf("%s 3D seams: %s\x1b[0m\n", mark, port_seam_fill ? "filled (as the DS)" : "off (faster)");
            continue;
        }
        if (w->cheat < 0) printf("%s%s %.34s\x1b[0m\n", mark, sFolders[w->folder].open ? "-" : "+", sFolders[w->folder].name);
        else printf("%s%s[%c] %.32s\x1b[0m\n", mark, w->folder >= 0 ? " " : "", sCheats[w->cheat].on ? 'x' : ' ', sCheats[w->cheat].name);
    }
}

// (debug.txt "menu F KEYS": at frame F the menu opens and is fed KEYS, comma-separated, one every
// 8 frames, as if pressed: A, B, UP, DOWN, LEFT, RIGHT. For testing it in an emulator.)
// (several such lines: a menu session each)
static u32 sScriptFrame[8], sScriptKeys[8][64];
static int sScriptLen[8], sScripts, sScriptPos, sScripted, sCurScript;
void port_cheats_script(u32 frame, const char *keys) {
    if (sScripts >= 8) return;
    int n = sScripts++;
    static const struct { const char *n; u32 k; } names[] = { { "A", KEY_A }, { "B", KEY_B }, { "UP", KEY_UP }, { "DOWN", KEY_DOWN }, { "LEFT", KEY_LEFT }, { "RIGHT", KEY_RIGHT } };
    sScriptFrame[n] = frame;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", keys);
    for (char *t = strtok(buf, ",\n\r "); t && sScriptLen[n] < 64; t = strtok(NULL, ",\n\r "))
        for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
            if (!strcmp(t, names[i].n)) sScriptKeys[n][sScriptLen[n]++] = names[i].k;
}
static void scan(u32 *held, u32 *down) {
    hidScanInput();
    *held = hidKeysHeld();
    *down = hidKeysDownRepeat();
    if (!sScripted) return;
    static int tick;
    *held = *down = 0;
    if (++tick % 8) return;
    if (sScriptPos < sScriptLen[sCurScript]) { *down = sScriptKeys[sCurScript][sScriptPos]; port_log("menu: key %d of %d\n", sScriptPos + 1, sScriptLen[sCurScript]); sScriptPos++; }
    else *down = KEY_B;
}

static int menu_keys(u32 held) {
    return (held & (KEY_ZL | KEY_ZR)) == (KEY_ZL | KEY_ZR) || (held & (KEY_L | KEY_R | KEY_SELECT)) == (KEY_L | KEY_R | KEY_SELECT);
}

static void menu(void) {
    void port_gpu2d_bottom_console(int console);
    port_gpu2d_bottom_console(1);
    static PrintConsole con;  // (static: it stays the console after the menu, for the log; on the stack it was a crash in the next printf)
    consoleInit(GFX_BOTTOM, &con);
    build_rows();
    int cur = 0, top = 0, changed = 0, redraw = 1;
    u32 held, down;
    while (aptMainLoop()) {  // (wait for the keys that opened it to be let go)
        scan(&held, &down);
        if (!menu_keys(held)) break;
        gspWaitForVBlank();
    }
    while (aptMainLoop()) {
        scan(&held, &down);
        if ((down & KEY_B) || menu_keys(held) || (down & KEY_START)) break;
        if (down & KEY_DOWN) { if (cur + 1 < sNumRows) cur++; redraw = 1; }
        if (down & KEY_UP) { if (cur > 0) cur--; redraw = 1; }
        if (down & KEY_RIGHT) { cur += 13; if (cur >= sNumRows) cur = sNumRows - 1; redraw = 1; }
        if (down & KEY_LEFT) { cur -= 13; if (cur < 0) cur = 0; redraw = 1; }
        if ((down & KEY_A) && sNumRows) {
            Row w = sRows[cur];
            if (w.folder == -2) {
                if (w.cheat == 0) port_display_fill ^= 1; else if (w.cheat == 1) port_side_fill ^= 1; else if (w.cheat == 2) port_stereo_enabled ^= 1; else port_seam_fill ^= 1;
                save_settings();
            } else if (w.cheat < 0) {
                sFolders[w.folder].open ^= 1;
                build_rows();
            } else {
                Cheat *c = &sCheats[w.cheat];
                c->on ^= 1;
                if (c->on && c->folder >= 0 && sFolders[c->folder].one)  // (one of this folder's at a time)
                    for (int i = 0; i < sNumCheats; i++) if (i != w.cheat && sCheats[i].folder == c->folder) sCheats[i].on = 0;
                changed = 1;
            }
            redraw = 1;
        }
        if (cur < top) top = cur;
        if (cur >= top + 26) top = cur - 25;
        if (redraw) { draw_menu(cur, top); redraw = 0; }
        gfxFlushBuffers();
        gfxScreenSwapBuffers(GFX_BOTTOM, false);
        gspWaitForVBlank();
    }
    while (aptMainLoop()) {  // (and for the keys that closed it, so the game doesn't see them)
        scan(&held, &down);
        if (!(held & (KEY_B | KEY_START | KEY_ZL | KEY_ZR | KEY_L | KEY_R | KEY_SELECT))) break;
        gspWaitForVBlank();
    }
    if (changed) save_choices();
    printf("\x1b[2J");
    port_gpu2d_bottom_console(0);
}

// Once a frame (port_frame, after the input): the menu if asked for, then the codes.
void port_cheats_frame(void) {
    extern volatile uint32_t port_frame_count;
    for (int i = 0; i < sScripts; i++)
        if (port_frame_count == sScriptFrame[i]) { sCurScript = i; sScriptPos = 0; sScripted = 1; menu(); sScripted = 0; port_log("menu: closed (frame %lu)\n", (unsigned long)port_frame_count); }
    if (menu_keys(hidKeysHeld())) menu();
    run_cheats();
}
