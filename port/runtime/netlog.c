// The log over Wi-Fi: every log line also goes, as a UDP packet, to a computer on the same
// network (tools/recv_log.py prints and saves them), so testing on a 3DS needs no SD card
// shuffling, and the last lines before a crash or freeze still arrive. On if the SD card has
// sdmc:/3ds/soulsilver-3ds/netlog.txt with the computer's address ("192.168.1.10", or
// "192.168.1.10:5555"; port 5555 by default). At start-up it also sends the previous run's last
// lines (kept as log.prev.txt) and a summary of any Luma crash dump not reported before.
#include <3ds.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <malloc.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT_DIR "sdmc:/3ds/soulsilver-3ds"
#define DUMPS "sdmc:/luma/dumps/arm11"

static int sSock = -1;
static struct sockaddr_in sTo;

void port_netlog_send(const char *s, int n) {
    if (sSock < 0 || n <= 0) return;
    sendto(sSock, s, (size_t)n, 0, (struct sockaddr *)&sTo, sizeof(sTo));
}

static void sendf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
    port_netlog_send(buf, n);
}

// The previous run's last lines (log.prev.txt), as "prev| ..." lines.
static void send_previous_log(void) {
    FILE *f = fopen(PORT_DIR "/log.prev.txt", "r");
    if (!f) return;
    static char lines[40][256];
    int n = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) { strcpy(lines[n % 40], line); n++; }
    fclose(f);
    sendf("prev| -- the previous run's last %d lines --\n", n < 40 ? n : 40);
    for (int i = n < 40 ? 0 : n - 40; i < n; i++) sendf("prev| %s", lines[i % 40]);
}

// The newest Luma crash dump, if not reported before (its name kept in lastdump.txt): its
// exception type and registers, which with the build ID resolve to the line (build-3ds/elves).
static void send_new_dump(void) {
    DIR *d = opendir(DUMPS);
    if (!d) return;
    char newest[64] = "";
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "crash_dump_", 11) || !strstr(e->d_name, ".dmp")) continue;
        if (strcmp(e->d_name, newest) > 0) snprintf(newest, sizeof(newest), "%s", e->d_name);
    }
    closedir(d);
    if (!newest[0]) return;
    char last[64] = "";
    FILE *lf = fopen(PORT_DIR "/lastdump.txt", "r");
    if (lf) { if (!fgets(last, sizeof(last), lf)) last[0] = 0; fclose(lf); last[strcspn(last, "\r\n")] = 0; }
    if (!strcmp(last, newest)) return;
    char path[128];
    snprintf(path, sizeof(path), DUMPS "/%s", newest);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    u8 b[0x28 + 23 * 4];
    size_t got = fread(b, 1, sizeof(b), f);
    fclose(f);
    if (got >= sizeof(b)) {
        u32 h[10], r[23];
        memcpy(h, b, sizeof(h));
        memcpy(r, b + 0x28, sizeof(r));
        static const char *kind[4] = { "FIQ", "undefined instruction", "prefetch abort", "data abort" };
        sendf("crash| %s: %s\n", newest, h[4] < 4 ? kind[h[4]] : "?");
        sendf("crash| PC %08lX LR %08lX SP %08lX FAR %08lX DFSR %lX\n", (unsigned long)r[15], (unsigned long)r[14], (unsigned long)r[13], (unsigned long)r[19], (unsigned long)r[17]);
        sendf("crash| R0-R6 %08lX %08lX %08lX %08lX %08lX %08lX %08lX\n", (unsigned long)r[0], (unsigned long)r[1], (unsigned long)r[2], (unsigned long)r[3], (unsigned long)r[4], (unsigned long)r[5], (unsigned long)r[6]);
    }
    lf = fopen(PORT_DIR "/lastdump.txt", "w");
    if (lf) { fputs(newest, lf); fclose(lf); }
}

void port_netlog_init(const char *build) {
    FILE *f = fopen(PORT_DIR "/netlog.txt", "r");
    if (!f) return;
    char line[64] = "";
    if (!fgets(line, sizeof(line), f)) line[0] = 0;
    fclose(f);
    line[strcspn(line, "\r\n ")] = 0;
    int port = 5555;
    char *colon = strchr(line, ':');
    if (colon) { *colon = 0; port = atoi(colon + 1); }
    memset(&sTo, 0, sizeof(sTo));
    sTo.sin_family = AF_INET;
    sTo.sin_port = htons((u16)port);
    if (!inet_aton(line, &sTo.sin_addr)) return;
    u32 *buf = memalign(0x1000, 0x40000);
    if (!buf || R_FAILED(socInit(buf, 0x40000))) return;
    sSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sSock < 0) return;
    sendf("== SoulSilver 3DS, build %s: log over Wi-Fi to %s:%d ==\n", build, line, port);
    send_previous_log();
    send_new_dump();
}
