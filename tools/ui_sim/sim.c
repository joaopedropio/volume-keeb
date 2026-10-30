// Renders the volume keymap's screens to PNG-ready raw frames on the PC by
// compiling the real keymap.c and Quantum Painter against an RGB565 surface.
// Usage: sim <outdir>  (writes <outdir>/<name>.rgb565, see run.py)
#include "keymap.c"

uint32_t sim_now = 1000;
static uint32_t last_input = 1000;
uint32_t last_input_activity_elapsed(void) { return sim_now - last_input; }
uint16_t sim_fb[LCD_WIDTH * LCD_HEIGHT];

static const char *outdir;

static void dump(const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.rgb565", outdir, name);
    FILE *f = fopen(path, "wb");
    fwrite(sim_fb, 2, LCD_WIDTH * LCD_HEIGHT, f);
    fclose(f);
}

static void tick(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i += 5) {
        sim_now += 5;
        housekeeping_task_user();
    }
}

typedef struct { const char *name; uint8_t vol; uint8_t flags; const char *title; } fake_app_t;
static const fake_app_t fake[] = {
    {"Master", 65, 0, NULL},
    {"Spotify", 80, 0x06, "Daft Punk - Harder, Better, Faster, Stronger"},
    {"Firefox", 40, 0x02, "Lo-fi beats to relax/study to - YouTube"},
    {"Discord", 100, 0x01, NULL},
    {"mpv", 55, 0x06, "Interstellar OST - No Time for Caution"},
    {"Steam", 30, 0, NULL},
    {"VLC media player with a long name", 70, 0, NULL},
};
#define FAKE_N (sizeof(fake) / sizeof(fake[0]))

static void send_snapshot(void) {
    uint8_t p[RAW_EPSIZE];
    memset(p, 0, sizeof(p));
    p[0] = APP_BEGIN_CMD; p[1] = FAKE_N;
    raw_hid_receive(p, sizeof(p));
    for (uint8_t i = 0; i < FAKE_N; i++) {
        memset(p, 0, sizeof(p));
        p[0] = APP_ENTRY_CMD; p[1] = i; p[2] = fake[i].vol; p[3] = fake[i].flags;
        uint8_t n = strlen(fake[i].name); if (n > RAW_EPSIZE - 5) n = RAW_EPSIZE - 5;
        p[4] = n; memcpy(&p[5], fake[i].name, n);
        raw_hid_receive(p, sizeof(p));
        if (fake[i].title) {
            size_t len = strlen(fake[i].title);
            for (size_t off = 0; off < len; off += RAW_EPSIZE - 3) {
                memset(p, 0, sizeof(p));
                p[0] = TITLE_CMD; p[1] = i; p[2] = off;
                size_t c = len - off; if (c > RAW_EPSIZE - 3) c = RAW_EPSIZE - 3;
                memcpy(&p[3], fake[i].title + off, c);
                raw_hid_receive(p, 3 + c);
            }
        }
    }
    memset(p, 0, sizeof(p));
    p[0] = APP_END_CMD; p[1] = FAKE_N;
    raw_hid_receive(p, sizeof(p));
}

static void turn(bool cw) { last_input = sim_now; encoder_update_user(0, cw); tick(20); }
static void click(void) {
    keyrecord_t r = {{true}};
    last_input = sim_now; process_record_user(ENC_CLICK, &r);
    tick(20);
    r.event.pressed = false; process_record_user(ENC_CLICK, &r);
    tick(DOUBLE_CLICK_MS + 50);
}

int main(int argc, char **argv) {
    outdir = argc > 1 ? argv[1] : ".";
    keyboard_post_init_user();
    char name[64];
    for (uint8_t th = 0; th < THEME_COUNT; th++) {
        uint8_t p[RAW_EPSIZE] = {THEME_CMD, th};
        raw_hid_receive(p, sizeof(p));
        send_snapshot();
        screen = SCREEN_MENU; menu_cursor = 0; active_app = 0; need_full_redraw = true;
        tick(20);
        turn(true); // cursor on Spotify
        snprintf(name, sizeof(name), "t%u_menu", th); dump(name);
        for (int i = 0; i < 4; i++) turn(true); // scrolled list
        snprintf(name, sizeof(name), "t%u_menu_scrolled", th); dump(name);
        for (int i = 0; i < 4; i++) turn(false);
        click(); // open Spotify
        for (int f = 0; f < 4; f++) {
            send_snapshot();
            snprintf(name, sizeof(name), "t%u_app_%d", th, f); dump(name);
            tick(1500);
        }
        click(); // back to menu
    }
    return 0;
}

// Animated images aren't used; satisfy qp_draw_image.c's references.
deferred_token defer_exec_advanced(deferred_executor_t *table, size_t table_count, uint32_t delay_ms, deferred_exec_callback callback, void *cb_arg) { return 0; }
bool cancel_deferred_exec_advanced(deferred_executor_t *table, size_t table_count, deferred_token token) { return false; }
void deferred_exec_advanced_task(deferred_executor_t *table, size_t table_count, uint32_t *last_execution_time) {}
