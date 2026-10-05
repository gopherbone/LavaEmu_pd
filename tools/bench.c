// Host benchmark of the VM without Python in the loop, built like the device
// (-Os):  make bench   (tools/bench GAMEDIR PROGRAM.lav)
// Runs 3600 frames (one virtual minute), Enter tapped every 2 s, and prints
// ops per frame and time per frame; the worst frame is the one with the most ops.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>

#include "../src/lava.h"

static uint8_t* slurp(const char* path, uint32_t* len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc(n ? n : 1);
    *len = (uint32_t)fread(b, 1, n, f);
    fclose(f);
    return b;
}

#ifdef LAVA_PROFILE
extern uint64_t lava_sys_count[256];
extern double lava_sys_time[256];
double lava_prof_now(void);
double lava_prof_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
#endif

static int cmpd(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}
static double p99(double* t, int n) {
    static double c[3600];
    memcpy(c, t, sizeof(double) * n);
    qsort(c, n, sizeof c[0], cmpd);
    return c[n * 99 / 100];
}

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    char p[1024];
    LavaFonts fonts = {0};
    const char* fn[4] = {"gbfont.bin", "gbfont16.bin", "ascii.bin", "ascii8.bin"};
    const uint8_t** fp[4] = {&fonts.gb12, &fonts.gb16, &fonts.asc12, &fonts.asc16};
    uint32_t* fl[4] = {&fonts.gb12_len, &fonts.gb16_len, &fonts.asc12_len, &fonts.asc16_len};
    for (int i = 0; i < 4; i++) {
        snprintf(p, sizeof p, "Source/fonts/%s", fn[i]);
        *fp[i] = slurp(p, fl[i]);
    }
    snprintf(p, sizeof p, "%s/%s", argv[1], argv[2]);
    uint32_t len;
    uint8_t* code = slurp(p, &len);
    static LavaVM vm;
    if (!code || !lava_init(&vm, code, len, &fonts)) return 1;
    if (argc > 3) lava_set_pace(&vm, (uint32_t)atoi(argv[3]));
    snprintf(p, sizeof p, "%s/LavaData", argv[1]);
    DIR* d = opendir(p);
    struct dirent* e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char q[1200], name[200];
        snprintf(q, sizeof q, "%s/%s", p, e->d_name);
        uint32_t n;
        uint8_t* data = slurp(q, &n);
        snprintf(name, sizeof name, "/LavaData/%s", e->d_name);
        lava_add_file(&vm, name, data, n, 0);
        free(data);
    }
    if (getenv("BOOTLOOP")) {     // profiling: the first 30 frames, over and over
        static LavaVM snap;
        snap = vm;
        for (int r = 0; r < atoi(getenv("BOOTLOOP")); r++) {
            vm = snap;
            for (int f = 0; f < 30; f++) lava_run_frame(&vm);
        }
        return 0;
    }
    static double times[3600];
    double worst_frame = 0;
    double total = 0, worst_t = 0;
    uint64_t worst_ops = 0, ops_total = 0, busy_ops = 0;
    double busy_t = 0;
    for (int f = 0; f < 3600; f++) {
        if (f % 120 == 60) lava_key_down(&vm, LK_ENTER);
        if (f % 120 == 66) lava_key_up(&vm, LK_ENTER, 0);
        uint64_t o = vm.ops;
        double t0 = now();
        lava_run_frame(&vm);
        double dt = now() - t0;
        uint64_t n = vm.ops - o;
        total += dt;
        times[f] = dt;
        if (dt > worst_frame) worst_frame = dt;
        ops_total += n;
        if (n > 600) busy_ops += n, busy_t += dt;
        if (n > worst_ops || (n == worst_ops && dt > worst_t)) worst_ops = n, worst_t = dt;
        if (getenv("SLOW") && dt > atof(getenv("SLOW")) * 1e-6) printf("  frame %d: %.1f us, %llu ops\n", f, dt * 1e6, (unsigned long long)n);
    }
    // the slowest 1% of frames
    printf("%-22s pace %2d: %5.0f ops/frame avg, %5.1f us/frame avg, p99 %6.1f us, max %6.1f us\n", argv[2],
           (int)vm.us_per_op, ops_total / 3600.0, total * 1e6 / 3600, p99(times, 3600) * 1e6, worst_frame * 1e6);
#ifdef LAVA_PROFILE
    for (int i = 0x80; i < 0xCB; i++)
        if (lava_sys_time[i] > 1e-4)
            printf("  sys %02x: %8llu calls, %7.2f ms total, %6.2f us each\n", i, (unsigned long long)lava_sys_count[i],
                   lava_sys_time[i] * 1e3, lava_sys_time[i] * 1e6 / lava_sys_count[i]);
#endif
    return 0;
}
