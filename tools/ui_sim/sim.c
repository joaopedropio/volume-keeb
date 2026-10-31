// Renders the volume keymap's screens to PNG-ready raw frames on the PC by
// compiling the real keymap.c and Quantum Painter against an RGB565 surface.
// Usage: sim <outdir>  (writes <outdir>/<name>.rgb565, see run.py)
#include "keymap.c"

uint32_t sim_now = 1000;
static uint32_t last_input = 1000;
uint32_t last_input_activity_elapsed(void) { return sim_now - last_input; }
uint16_t sim_fb[LCD_WIDTH * LCD_HEIGHT];

void tap_code(uint8_t code) {
    printf("key %s\n", code == KC_AUDIO_VOL_UP ? "vol+" : code == KC_AUDIO_VOL_DOWN ? "vol-" : code == KC_AUDIO_MUTE ? "mute" : "?");
}

static uint8_t sim_eeprom[EECONFIG_USER_DATA_SIZE];
static bool    sim_eeprom_valid;
bool eeconfig_is_user_datablock_valid(void) { return sim_eeprom_valid; }
uint32_t eeconfig_read_user_datablock(void *data, uint32_t offset, uint32_t length) { memcpy(data, sim_eeprom + offset, length); return length; }
uint32_t eeconfig_update_user_datablock(const void *data, uint32_t offset, uint32_t length) { memcpy(sim_eeprom + offset, data, length); sim_eeprom_valid = true; return length; }

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

static void hold(void) {
    keyrecord_t r = {{true}};
    last_input = sim_now; process_record_user(ENC_CLICK, &r);
    tick(HOLD_MS + 50);
    r.event.pressed = false; process_record_user(ENC_CLICK, &r);
    tick(20);
}

static void turn(bool cw) { last_input = sim_now; encoder_update_user(0, cw); tick(20); }
static void click(void) {
    keyrecord_t r = {{true}};
    last_input = sim_now; process_record_user(ENC_CLICK, &r);
    tick(20);
    r.event.pressed = false; process_record_user(ENC_CLICK, &r);
    tick(DOUBLE_CLICK_MS + 50);
}

// SCREEN_CMD with the current screensaver settings and menu layout `m`.
static void set_menu(uint8_t m) {
    uint16_t after = saver_after_ms / 1000, off = off_after_ms / 1000;
    uint8_t  p[RAW_EPSIZE] = {SCREEN_CMD, saver_kind, after & 0xFF, after >> 8, off & 0xFF, off >> 8, 0, m};
    raw_hid_receive(p, sizeof(p));
    tick(20);
}

static void double_click(void) {
    keyrecord_t r = {{true}};
    last_input = sim_now; process_record_user(ENC_CLICK, &r);
    tick(20);
    r.event.pressed = false; process_record_user(ENC_CLICK, &r);
    tick(60);
    r.event.pressed = true; process_record_user(ENC_CLICK, &r);
    tick(20);
    r.event.pressed = false; process_record_user(ENC_CLICK, &r);
    tick(DOUBLE_CLICK_MS + 50);
}

int main(int argc, char **argv) {
    outdir = argc > 1 ? argv[1] : ".";
    keyboard_post_init_user();
    char name[64];

    // Boot with no host app: loading, then the screensaver as home, where
    // turning and holding are media keys and a click opens the settings.
    tick(1000); dump("loading_1s");
    tick(2600); dump("loading_3s");
    tick(2000); dump("nohost_home");
    printf("expect vol+ vol- mute:\n");
    turn(true); turn(false); hold();
    tick(200); dump("nohost_home_after_keys"); // still the screensaver
    click(); dump("nohost_settings");
    turn(true); dump("nohost_cursor");          // on Screensaver
    click(); dump("nohost_editing");            // editing it
    turn(false); dump("nohost_changed");        // drive -> none
    click(); turn(true); turn(true); turn(true); // leave, down to Exit
    dump("nohost_exit_row");
    turn(false); turn(false); turn(false); turn(false); click(); turn(false); click(); // theme back one: neon -> winamp
    dump("nohost_theme");
    hold(); dump("nohost_home_none");           // held: home, no screensaver
    click(); turn(true); click(); turn(true); click(); // screensaver none -> drive
    turn(true); turn(true); turn(true); click(); // Exit
    tick(300); dump("nohost_home_drive");
    for (uint8_t th = 0; th < THEME_COUNT; th++) {
        uint8_t p[RAW_EPSIZE] = {THEME_CMD, th};
        raw_hid_receive(p, sizeof(p));
        send_snapshot();
        screen = SCREEN_MENU; menu_cursor = 0; active_app = 0; need_full_redraw = true;
        tick(20);
        turn(true); // cursor on Spotify
        snprintf(name, sizeof(name), "t%u_menu", th); dump(name);
        set_menu(MENU_LIST); snprintf(name, sizeof(name), "t%u_menu_list", th); dump(name);
        set_menu(MENU_MIXER); snprintf(name, sizeof(name), "t%u_menu_mixer", th); dump(name);
        set_menu(MENU_CAROUSEL);
        for (int i = 0; i < 4; i++) turn(true); // scrolled list
        snprintf(name, sizeof(name), "t%u_menu_scrolled", th); dump(name);
        for (int i = 0; i < 4; i++) turn(false);
        click(); // open Spotify
        for (int f = 0; f < 4; f++) {
            send_snapshot();
            snprintf(name, sizeof(name), "t%u_app_%d", th, f); dump(name);
            tick(1500);
        }
        hold(); snprintf(name, sizeof(name), "t%u_app_muted", th); dump(name);
        hold();
        click(); // back to menu
        turn(false); click(); // Master, nothing playing
        snprintf(name, sizeof(name), "t%u_app_nomedia", th); dump(name);
        click();
    }

    // The mixer menu sets volumes: click grabs Spotify's fader, turning
    // moves it, a click lets go; holding mutes, a double click opens it.
    send_snapshot(); set_menu(MENU_MIXER);
    screen = SCREEN_MENU; menu_cursor = 1; need_full_redraw = true; tick(20);
    click(); dump("mixer_grabbed");
    turn(true); turn(true); dump("mixer_turned");
    printf("Spotify's volume, expect 90: %u\n", apps[1].volume);
    click(); turn(true); dump("mixer_let_go"); // the cursor moves again
    send_snapshot(); // the host's next update, so it doesn't look gone
    hold(); dump("mixer_muted");
    double_click(); dump("mixer_opened");
    click(); set_menu(MENU_CAROUSEL);

    // Idle long enough for the screensaver, then the drive along its whole
    // route (four straights and turns), the sky sliding with the turns.
    tick(SAVER_AFTER_MS_DEFAULT + 100);
    for (int f = 0; f < 12; f++) {
        snprintf(name, sizeof(name), "saver_%d", f); dump(name);
        tick(3000);
    }
    turn(true); // the knob stops it without changing anything
    tick(20);
    dump("saver_stopped");

    // Every other screensaver, previewed from the host, in two themes.
    static const char *const saver_names[] = {"none", "drive", "stars", "rain", "plasma", "bounce"};
    static const uint8_t saver_themes[] = {3, 1};
    for (uint8_t ti = 0; ti < sizeof(saver_themes); ti++) {
        uint8_t th[RAW_EPSIZE] = {THEME_CMD, saver_themes[ti]};
        raw_hid_receive(th, sizeof(th));
        for (uint8_t k = SAVER_STARS; k < SAVER_GIF1; k++) {
            uint8_t p[RAW_EPSIZE] = {SCREEN_CMD, k, 30, 0, 0, 0, 1};
            raw_hid_receive(p, sizeof(p));
            for (int f = 0; f < 3; f++) {
                send_snapshot(); // keeps the host fresh, so the bounce has a title
                tick(f ? 1500 : 600);
                snprintf(name, sizeof(name), "saver_%s_t%u_%d", saver_names[k], saver_themes[ti], f); dump(name);
            }
            turn(true);
            tick(20);
        }
    }

    // The gif savers whose slots hold an animation (UI_SIM_FONTS), a few
    // frames each, 100 ms apart.
    for (uint8_t i = 0; i < ANIM_SLOTS; i++) {
        if (!anim_ok[i]) continue;
        uint8_t p[RAW_EPSIZE] = {SCREEN_CMD, SAVER_GIF1 + i, 30, 0, 0, 0, 1};
        raw_hid_receive(p, sizeof(p));
        for (int f = 0; f < 6; f++) {
            tick(f ? 100 : 10);
            snprintf(name, sizeof(name), "saver_gif%u_%d", i + 1, f); dump(name);
        }
        turn(true);
        tick(20);
    }

    // Off after 60 s with no screensaver: the UI stays until then.
    uint8_t p[RAW_EPSIZE] = {SCREEN_CMD, SAVER_NONE, 30, 0, 60, 0, 0};
    raw_hid_receive(p, sizeof(p));
    tick(45000);
    dump("idle_45s_no_saver");
    tick(20000);
    dump("idle_65s_off");
    return 0;
}

// Animated images aren't used; satisfy qp_draw_image.c's references.
deferred_token defer_exec_advanced(deferred_executor_t *table, size_t table_count, uint32_t delay_ms, deferred_exec_callback callback, void *cb_arg) { return 0; }
bool cancel_deferred_exec_advanced(deferred_executor_t *table, size_t table_count, deferred_token token) { return false; }
void deferred_exec_advanced_task(deferred_executor_t *table, size_t table_count, uint32_t *last_execution_time) {}
