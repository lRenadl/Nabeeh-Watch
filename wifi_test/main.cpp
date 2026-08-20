/**
 * Nabeeh watch — full integration: the LVGL UI screens (originally
 * main_ui_backup.cpp) plus the Wi-Fi/TCP channel (mic audio out, control
 * and now classification results in).
 *
 * Audio format: 16 kHz, 16-bit, mono PCM — the mic's native format, so no
 * resampling is needed. ~32 KB/s, trivial for local Wi-Fi TCP.
 *
 * Wire protocol (one TCP connection, port 3333, full-duplex):
 *   watch -> phone : raw PCM audio bytes, continuously, while streaming
 *   phone -> watch : single control bytes
 *     'r' / 'R'  -> start streaming mic audio               (unchanged)
 *     's' / 'S'  -> stop streaming mic audio                (unchanged)
 *     '#' + code -> classification result arrived; show it on the watch
 *                   (new — see result_codes[] below for the category
 *                   letters). '#' was picked instead of reusing 'R' for
 *                   this because 'R' already means "start streaming" above
 *                   and reusing it would collide.
 * Same commands work over USB Serial too (dev/testing convenience) — e.g.
 * typing "#B" in the Serial Monitor simulates a "باكاء أطفال" result
 * without needing the phone to send anything for real.
 *
 * Build/upload with: pio run -e wifi_test -t upload -t monitor
 */
#include <LilyGoLib.h>
#include <LV_Helper.h>
#include <WiFi.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <Preferences.h>
#include "ui_fonts.h"
#include "ui_images.h"

#define COLOR_PRIMARY lv_color_hex(0x181059)
#define COLOR_BG      lv_color_hex(0x0B0B1A)
#define COLOR_TEXT    lv_color_white()
#define COLOR_MUTED   lv_color_hex(0xA0A0B8)

static const char *arabic_month_names[] = {
    "يناير", "فبراير", "مارس", "أبريل", "مايو", "يونيو",
    "يوليو", "أغسطس", "سبتمبر", "أكتوبر", "نوفمبر", "ديسمبر",
};

// Writes the Arabic-Indic digits (٠-٩) for n (0-99) into out, UTF-8, NUL-terminated.
// Two-digit numbers are written ones-digit-first: LVGL's BIDI engine visually
// reverses a two-codepoint Arabic-Indic number inside RTL text (e.g. the day
// number sitting next to an Arabic month name), so "١٦" in memory order was
// showing up on screen as "٦١". Swapping the write order here cancels that
// out and displays correctly, without having to fight LVGL's BIDI engine.
static void arabic_digits(int n, char *out)
{
    static const char *digit[10] = {"٠", "١", "٢", "٣", "٤", "٥", "٦", "٧", "٨", "٩"};
    out[0] = '\0';
    if (n >= 10) {
        strcat(out, digit[n % 10]);
        strcat(out, digit[n / 10]);
    } else {
        strcat(out, digit[n % 10]);
    }
}

struct AlertCategory {
    const lv_image_dsc_t *icon;
    const lv_image_dsc_t *sign_gif;
    const char *label;
};

static const AlertCategory alert_categories[] = {
    {&emoji_bell,   &sign_doorbell, "جرس الباب"},
    {&emoji_knock,  &sign_knock,    "طرق على الباب"},
    {&emoji_baby,   &sign_baby,     "بكاء أطفال"},
    {&emoji_fire,   &sign_fire,     "إنذار حريق"},
    {&emoji_mosque, &sign_adhan,    "الأذان"},
};
#define ALERT_CATEGORY_COUNT (sizeof(alert_categories) / sizeof(alert_categories[0]))

// Maps a single-byte result code (sent after '#') to an alert_categories index.
struct ResultCode {
    char code;
    int category_index;
};
static const ResultCode result_codes[] = {
    {'D', 0}, // جرس الباب
    {'K', 1}, // طرق على الباب
    {'B', 2}, // بكاء أطفال
    {'F', 3}, // إنذار حريق
    {'A', 4}, // الأذان
};
#define RESULT_CODE_COUNT (sizeof(result_codes) / sizeof(result_codes[0]))

static lv_obj_t *splash_screen;
static lv_obj_t *onboarding_screen;
static lv_obj_t *home_screen;
static lv_obj_t *settings_screen = NULL;
static lv_obj_t *alert_screen = NULL;
static lv_obj_t *clock_label;
static lv_obj_t *date_label;
static lv_obj_t *batt_pct_label;
static lv_obj_t *batt_icon_label;
static lv_obj_t *status_label;
static lv_obj_t *status_dot;
static bool sign_language_mode = true; // matches the pre-selected option in onboarding
static bool phone_connected = true;

#define COLOR_CONNECTED    lv_color_hex(0x35D07F)
#define COLOR_DISCONNECTED lv_color_hex(0xE05A4E)

static void build_settings_screen();
static void change_wifi_cb(lv_event_t *e); // defined near start_ble_provisioning(), further down

static lv_obj_t *seg_text_btn, *seg_text_lbl_ref;
static lv_obj_t *seg_sign_btn, *seg_sign_lbl_ref;

static void select_segment(lv_obj_t *selected_btn, lv_obj_t *selected_lbl,
                            lv_obj_t *other_btn, lv_obj_t *other_lbl)
{
    lv_obj_set_style_bg_opa(selected_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(selected_btn, lv_color_white(), 0);
    lv_obj_set_style_text_color(selected_lbl, COLOR_PRIMARY, 0);

    lv_obj_set_style_bg_opa(other_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(other_lbl, COLOR_TEXT, 0);
}

static void select_text_mode_cb(lv_event_t *e)
{
    sign_language_mode = false;
    select_segment(seg_text_btn, seg_text_lbl_ref, seg_sign_btn, seg_sign_lbl_ref);
}

static void select_sign_mode_cb(lv_event_t *e)
{
    sign_language_mode = true;
    select_segment(seg_sign_btn, seg_sign_lbl_ref, seg_text_btn, seg_text_lbl_ref);
}

static void go_to_settings_cb(lv_event_t *e)
{
    if (!settings_screen) {
        build_settings_screen();
    }
    // re-sync the toggle every visit, in case the user re-ran onboarding
    // (via the restart button) and picked differently since settings was built
    if (sign_language_mode) {
        select_segment(seg_sign_btn, seg_sign_lbl_ref, seg_text_btn, seg_text_lbl_ref);
    } else {
        select_segment(seg_text_btn, seg_text_lbl_ref, seg_sign_btn, seg_sign_lbl_ref);
    }
    lv_screen_load_anim(settings_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

static void go_to_home_cb(lv_event_t *e)
{
    lv_screen_load_anim(home_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}

static void update_connection_status()
{
    lv_label_set_text(status_label, phone_connected ? "متصلة" : "غير متصلة");
    lv_obj_set_style_bg_color(status_dot, phone_connected ? COLOR_CONNECTED : COLOR_DISCONNECTED, 0);
}

static lv_obj_t *confirm_modal_overlay = NULL;

static void close_confirm_modal_cb(lv_event_t *e)
{
    if (confirm_modal_overlay) {
        lv_obj_delete(confirm_modal_overlay);
        confirm_modal_overlay = NULL;
    }
}

static void confirm_disconnect_cb(lv_event_t *e)
{
    phone_connected = false;
    update_connection_status();
    close_confirm_modal_cb(e);
}

static void show_disconnect_confirm_cb(lv_event_t *e)
{
    lv_obj_t *parent = lv_screen_active();

    confirm_modal_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(confirm_modal_overlay);
    lv_obj_set_size(confirm_modal_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(confirm_modal_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(confirm_modal_overlay, LV_OPA_60, 0);
    lv_obj_center(confirm_modal_overlay);

    lv_obj_t *card = lv_obj_create(confirm_modal_overlay);
    lv_obj_set_size(card, lv_pct(85), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, COLOR_PRIMARY, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 12, 0);
    lv_obj_center(card);

    lv_obj_t *question = lv_label_create(card);
    lv_label_set_text(question, "هل فعلاً تبي تفصل الساعة عن الجوال؟");
    lv_obj_set_style_text_font(question, &tajawal_regular_16, 0);
    lv_obj_set_style_text_color(question, COLOR_TEXT, 0);
    lv_obj_set_width(question, lv_pct(100));
    lv_obj_set_style_text_align(question, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *yes_btn = lv_button_create(card);
    lv_obj_set_size(yes_btn, lv_pct(100), 36);
    lv_obj_set_style_bg_color(yes_btn, COLOR_DISCONNECTED, 0);
    lv_obj_set_style_bg_opa(yes_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(yes_btn, 10, 0);
    lv_obj_add_event_cb(yes_btn, confirm_disconnect_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *yes_lbl = lv_label_create(yes_btn);
    lv_label_set_text(yes_lbl, "نعم، افصل");
    lv_obj_set_style_text_font(yes_lbl, &tajawal_regular_16, 0);
    lv_obj_set_style_text_color(yes_lbl, lv_color_white(), 0);
    lv_obj_center(yes_lbl);

    lv_obj_t *no_btn = lv_button_create(card);
    lv_obj_set_size(no_btn, lv_pct(100), 36);
    lv_obj_set_style_bg_opa(no_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(no_btn, lv_color_white(), 0);
    lv_obj_set_style_border_opa(no_btn, LV_OPA_40, 0);
    lv_obj_set_style_border_width(no_btn, 1, 0);
    lv_obj_set_style_radius(no_btn, 10, 0);
    lv_obj_add_event_cb(no_btn, close_confirm_modal_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *no_lbl = lv_label_create(no_btn);
    lv_label_set_text(no_lbl, "لا");
    lv_obj_set_style_text_font(no_lbl, &tajawal_regular_16, 0);
    lv_obj_set_style_text_color(no_lbl, COLOR_TEXT, 0);
    lv_obj_center(no_lbl);
}

static void splash_timeout_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    lv_screen_load_anim(onboarding_screen, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, false);
}

static void restart_watch_cb(lv_event_t *e)
{
    // A real ESP.restart() froze the panel on this hardware (a software
    // reset doesn't cleanly re-init the display the way a power-on/hard
    // reset does), so this "restart" just replays the boot UI flow instead.
    lv_screen_load_anim(splash_screen, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, false);
    lv_timer_t *t = lv_timer_create(splash_timeout_cb, 4000, NULL);
    lv_timer_set_repeat_count(t, 1);
}

static void choose_sign_language_cb(lv_event_t *e)
{
    sign_language_mode = true;
    lv_screen_load_anim(home_screen, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, false);
}

static void choose_text_mode_cb(lv_event_t *e)
{
    sign_language_mode = false;
    lv_screen_load_anim(home_screen, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, false);
}

static lv_obj_t *make_screen()
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, COLOR_BG, 0);
    lv_obj_set_style_bg_grad_color(scr, COLOR_PRIMARY, 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 12, 0);
    return scr;
}

static void build_splash_screen()
{
    splash_screen = make_screen();

    lv_obj_t *logo = lv_image_create(splash_screen);
    lv_image_set_src(logo, &nabeeh_logo);
    lv_obj_center(logo);

    lv_timer_t *t = lv_timer_create(splash_timeout_cb, 4000, NULL);
    lv_timer_set_repeat_count(t, 1);
}

static void build_onboarding_screen()
{
    onboarding_screen = make_screen();

    lv_obj_t *question = lv_label_create(onboarding_screen);
    lv_label_set_text(question, "كيف تحب تشوف الأصوات المكتشفة؟");
    lv_obj_set_style_text_font(question, &tajawal_regular_16, 0);
    lv_obj_set_style_text_color(question, COLOR_TEXT, 0);
    lv_obj_set_width(question, lv_pct(90));
    lv_obj_set_style_text_align(question, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(question, LV_ALIGN_TOP_MID, 0, 30);

    lv_obj_t *sign_btn = lv_button_create(onboarding_screen);
    lv_obj_set_size(sign_btn, lv_pct(90), 44);
    lv_obj_set_style_bg_color(sign_btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(sign_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sign_btn, 10, 0);
    lv_obj_align_to(sign_btn, question, LV_ALIGN_OUT_BOTTOM_MID, 0, 24);
    lv_obj_add_event_cb(sign_btn, choose_sign_language_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sign_lbl = lv_label_create(sign_btn);
    lv_label_set_text(sign_lbl, "لغة الإشارة");
    lv_obj_set_style_text_font(sign_lbl, &tajawal_regular_16, 0);
    lv_obj_set_style_text_color(sign_lbl, COLOR_PRIMARY, 0);
    lv_obj_center(sign_lbl);

    lv_obj_t *text_btn = lv_button_create(onboarding_screen);
    lv_obj_set_size(text_btn, lv_pct(90), 44);
    lv_obj_set_style_bg_opa(text_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(text_btn, lv_color_white(), 0);
    lv_obj_set_style_border_opa(text_btn, LV_OPA_30, 0);
    lv_obj_set_style_border_width(text_btn, 1, 0);
    lv_obj_set_style_radius(text_btn, 10, 0);
    lv_obj_align_to(text_btn, sign_btn, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
    lv_obj_add_event_cb(text_btn, choose_text_mode_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *text_lbl = lv_label_create(text_btn);
    lv_label_set_text(text_lbl, "نص / أيقونة");
    lv_obj_set_style_text_font(text_lbl, &tajawal_regular_16, 0);
    lv_obj_set_style_text_color(text_lbl, COLOR_TEXT, 0);
    lv_obj_center(text_lbl);
}

static void build_home_screen()
{
    home_screen = make_screen();

    // top-left: settings icon
    lv_obj_t *settings_btn = lv_button_create(home_screen);
    lv_obj_set_style_bg_opa(settings_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(settings_btn, 0, 0);
    lv_obj_set_style_pad_all(settings_btn, 4, 0);
    lv_obj_align(settings_btn, LV_ALIGN_TOP_LEFT, -4, -4);
    lv_obj_add_event_cb(settings_btn, go_to_settings_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *settings_icon = lv_label_create(settings_btn);
    lv_label_set_text(settings_icon, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(settings_icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(settings_icon, COLOR_TEXT, 0);

    // top-right: connection status (dot sits to the right of the Arabic word)
    lv_obj_t *status_row = lv_obj_create(home_screen);
    lv_obj_remove_style_all(status_row);
    lv_obj_set_size(status_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(status_row, LV_ALIGN_TOP_RIGHT, 0, 4);
    lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_row, 6, 0);

    status_label = lv_label_create(status_row);
    lv_obj_set_style_text_font(status_label, &tajawal_regular_16, 0);
    lv_obj_set_style_text_color(status_label, COLOR_TEXT, 0);

    status_dot = lv_obj_create(status_row);
    lv_obj_remove_style_all(status_dot);
    lv_obj_set_size(status_dot, 8, 8);
    lv_obj_set_style_radius(status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(status_dot, LV_OPA_COVER, 0);

    // center: clock
    clock_label = lv_label_create(home_screen);
    lv_label_set_text(clock_label, "10:24");
    lv_obj_set_style_text_font(clock_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(clock_label, COLOR_TEXT, 0);
    lv_obj_align(clock_label, LV_ALIGN_CENTER, 0, -10);

    // date
    date_label = lv_label_create(home_screen);
    lv_label_set_text(date_label, "٨ أغسطس");
    lv_obj_set_style_text_font(date_label, &tajawal_regular_16, 0);
    lv_obj_set_style_text_color(date_label, COLOR_MUTED, 0);
    lv_obj_align_to(date_label, clock_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    // battery, bottom
    lv_obj_t *batt_row = lv_obj_create(home_screen);
    lv_obj_remove_style_all(batt_row);
    lv_obj_remove_flag(batt_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(batt_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(batt_row, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_flex_flow(batt_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(batt_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(batt_row, 6, 0);

    batt_pct_label = lv_label_create(batt_row);
    lv_obj_set_style_text_font(batt_pct_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(batt_pct_label, COLOR_MUTED, 0);

    batt_icon_label = lv_label_create(batt_row);
    lv_obj_set_style_text_color(batt_icon_label, COLOR_MUTED, 0);
}

static void update_clock_display_cb(lv_timer_t *t)
{
    RTC_DateTime now = instance.rtc.getDateTime();

    char time_buf[8];
    snprintf(time_buf, sizeof(time_buf), "%02d:%02d", now.getHour(), now.getMinute());
    lv_label_set_text(clock_label, time_buf);

    char day_buf[8];
    arabic_digits(now.getDay(), day_buf);
    char date_buf[40];
    snprintf(date_buf, sizeof(date_buf), "%s %s", day_buf,
             arabic_month_names[now.getMonth() >= 1 && now.getMonth() <= 12 ? now.getMonth() - 1 : 0]);
    lv_label_set_text(date_label, date_buf);

    int batt_pct = instance.pmu.getBatteryPercent();
    if (batt_pct >= 0) {
        char pct_buf[8];
        snprintf(pct_buf, sizeof(pct_buf), "%d%%", batt_pct);
        lv_label_set_text(batt_pct_label, pct_buf);
    } else {
        lv_label_set_text(batt_pct_label, "--%");
        batt_pct = 0;
    }

    if (instance.pmu.isCharging()) {
        lv_label_set_text(batt_icon_label, LV_SYMBOL_CHARGE);
    } else if (batt_pct >= 90) {
        lv_label_set_text(batt_icon_label, LV_SYMBOL_BATTERY_FULL);
    } else if (batt_pct >= 60) {
        lv_label_set_text(batt_icon_label, LV_SYMBOL_BATTERY_3);
    } else if (batt_pct >= 40) {
        lv_label_set_text(batt_icon_label, LV_SYMBOL_BATTERY_2);
    } else if (batt_pct >= 15) {
        lv_label_set_text(batt_icon_label, LV_SYMBOL_BATTERY_1);
    } else {
        lv_label_set_text(batt_icon_label, LV_SYMBOL_BATTERY_EMPTY);
    }
}

static lv_obj_t *alert_pulse_ring;
static lv_obj_t *alert_icon_img;
static lv_obj_t *alert_category_lbl;
static lv_anim_t alert_pulse_anim;

static void alert_pulse_anim_cb(void *obj, int32_t v)
{
    // v goes 0 -> 100: ring grows from 130px to 195px while fading out.
    // Resize + re-center every frame (instead of a transform scale) so it
    // can never drift off the static circle's center.
    lv_obj_t *ring = (lv_obj_t *)obj;
    int32_t sz = 130 + (v * 65) / 100;                // 130 .. 195 px
    lv_opa_t opa = 130 - (lv_opa_t)((v * 130) / 100);  // 130 .. 0
    lv_obj_set_size(ring, sz, sz);
    lv_obj_center(ring);
    lv_obj_set_style_bg_opa(ring, opa, 0);
}

static void build_alert_screen()
{
    alert_screen = make_screen();

    lv_obj_t *back_btn = lv_button_create(alert_screen);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_set_style_pad_all(back_btn, 4, 0);
    lv_obj_align(back_btn, LV_ALIGN_TOP_RIGHT, 4, -4);
    lv_obj_add_event_cb(back_btn, go_to_home_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_icon = lv_label_create(back_btn);
    lv_label_set_text(back_icon, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(back_icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(back_icon, COLOR_TEXT, 0);

    alert_pulse_ring = lv_obj_create(alert_screen);
    lv_obj_remove_style_all(alert_pulse_ring);
    lv_obj_remove_flag(alert_pulse_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(alert_pulse_ring, 130, 130);
    lv_obj_set_style_radius(alert_pulse_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(alert_pulse_ring, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(alert_pulse_ring, LV_OPA_TRANSP, 0);
    lv_obj_center(alert_pulse_ring);

    lv_obj_t *icon_bg = lv_obj_create(alert_screen);
    lv_obj_remove_style_all(icon_bg);
    lv_obj_remove_flag(icon_bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(icon_bg, 110, 110);
    lv_obj_set_style_radius(icon_bg, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(icon_bg, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(icon_bg, LV_OPA_20, 0);
    lv_obj_center(icon_bg);

    alert_icon_img = lv_image_create(alert_screen);
    lv_obj_center(alert_icon_img);

    alert_category_lbl = lv_label_create(alert_screen);
    lv_obj_set_style_text_font(alert_category_lbl, &tajawal_bold_24, 0);
    lv_obj_set_style_text_color(alert_category_lbl, COLOR_TEXT, 0);
    lv_obj_set_width(alert_category_lbl, lv_pct(90));
    lv_obj_set_style_text_align(alert_category_lbl, LV_TEXT_ALIGN_CENTER, 0);
    // the ring fades to fully transparent by the time it's at its largest,
    // so only its early (small, still-opaque) size needs real clearance
    lv_obj_align(alert_category_lbl, LV_ALIGN_CENTER, 0, 100);

    lv_anim_init(&alert_pulse_anim);
    lv_anim_set_var(&alert_pulse_anim, alert_pulse_ring);
    lv_anim_set_exec_cb(&alert_pulse_anim, alert_pulse_anim_cb);
    lv_anim_set_values(&alert_pulse_anim, 0, 100);
    lv_anim_set_duration(&alert_pulse_anim, 1400);
    lv_anim_set_repeat_count(&alert_pulse_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&alert_pulse_anim, lv_anim_path_ease_out);
}

static lv_obj_t *sign_screen = NULL;
static lv_obj_t *sign_gif_widget;
static lv_obj_t *sign_category_lbl;

static void build_sign_screen()
{
    sign_screen = make_screen();

    lv_obj_t *back_btn = lv_button_create(sign_screen);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_set_style_pad_all(back_btn, 4, 0);
    lv_obj_align(back_btn, LV_ALIGN_TOP_RIGHT, 4, -4);
    lv_obj_add_event_cb(back_btn, go_to_home_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_icon = lv_label_create(back_btn);
    lv_label_set_text(back_icon, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(back_icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(back_icon, COLOR_TEXT, 0);

    lv_obj_t *frame = lv_obj_create(sign_screen);
    lv_obj_remove_flag(frame, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(frame, 170, 170);
    lv_obj_set_style_radius(frame, 16, 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(frame, lv_color_white(), 0);
    lv_obj_set_style_border_opa(frame, LV_OPA_40, 0);
    lv_obj_set_style_border_width(frame, 1, 0);
    lv_obj_set_style_pad_all(frame, 0, 0);
    lv_obj_set_style_clip_corner(frame, true, 0);
    lv_obj_align(frame, LV_ALIGN_CENTER, 0, -20);

    sign_gif_widget = lv_gif_create(frame);
    lv_gif_set_color_format(sign_gif_widget, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_center(sign_gif_widget);

    sign_category_lbl = lv_label_create(sign_screen);
    lv_obj_set_style_text_font(sign_category_lbl, &tajawal_bold_24, 0);
    lv_obj_set_style_text_color(sign_category_lbl, COLOR_TEXT, 0);
    lv_obj_set_width(sign_category_lbl, lv_pct(90));
    lv_obj_set_style_text_align(sign_category_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(sign_category_lbl, frame, LV_ALIGN_OUT_BOTTOM_MID, 0, 12);
}

static void show_sign_result(int category_index)
{
    if (!sign_screen) {
        build_sign_screen();
    }
    const AlertCategory &cat = alert_categories[category_index];
    lv_gif_set_src(sign_gif_widget, cat.sign_gif);
    lv_label_set_text(sign_category_lbl, cat.label);
    lv_screen_load_anim(sign_screen, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
}

static void show_alert(int category_index)
{
    if (sign_language_mode) {
        show_sign_result(category_index);
        return;
    }

    if (!alert_screen) {
        build_alert_screen();
    }
    const AlertCategory &cat = alert_categories[category_index];
    lv_image_set_src(alert_icon_img, cat.icon);
    lv_label_set_text(alert_category_lbl, cat.label);
    lv_screen_load_anim(alert_screen, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
    lv_anim_delete(alert_pulse_ring, alert_pulse_anim_cb);
    lv_anim_start(&alert_pulse_anim);
}

// Auto-return to Home a few seconds after a result is shown, so a detected
// sound doesn't sit on screen forever waiting for the user to back out.
static lv_timer_t *alert_return_timer = NULL;
#define ALERT_AUTO_RETURN_MS 6000

static void alert_return_timeout_cb(lv_timer_t *t)
{
    alert_return_timer = NULL;
    lv_screen_load_anim(home_screen, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
}

// Only the UI task (the one calling lv_timer_handler() in loop()) is allowed
// to touch LVGL objects — LVGL isn't thread-safe. It also turns out to be
// the only task that's allowed to touch the DRV2605 haptic driver: it's I2C,
// on the SAME bus as the RTC/PMU reads the UI task already does on its own
// 30s timer — calling instance.vibrator() from the network task raced that
// bus and wedged it hard enough to freeze the whole screen (not a
// hypothetical: this actually happened while testing). So handle_result_code()
// below, which can run on either task, never touches LVGL or the haptic
// driver directly — it only ever hands a full result (category + pattern +
// intensity) off through this queue; draining it and actually acting on it
// happens back in loop(), on the UI task, every time.
struct ResultMessage {
    int category_index;
    char pattern;
    char intensity;
};
static QueueHandle_t result_queue;

static void show_result_category(int category_index)
{
    show_alert(category_index);
    if (alert_return_timer) {
        lv_timer_delete(alert_return_timer);
    }
    alert_return_timer = lv_timer_create(alert_return_timeout_cb, ALERT_AUTO_RETURN_MS, NULL);
    lv_timer_set_repeat_count(alert_return_timer, 1);
}

// Vibration motor is a DRV2605L haptic driver (I2C, LilyGoLib's
// instance.setHapticEffects()+instance.vibrator()), not a plain PWM motor —
// "strength" and "pattern" both have to be picked from its built-in
// waveform library, not set as a raw number. Effect IDs below are copied
// from SensorLib's own DRV2605_Basic.ino example (the TI DRV2605L Library 1
// waveform table), not guessed. Three shapes (نمط) x three intensities
// (قوة) each, all real, verified table entries:
//   نمط '1' Strong Click : 100%/60%/30% -> effects 1/2/3
//   نمط '2' Buzz         : 100%/60%/20% -> effects 47/49/51
//   نمط '3' Soft Bump    : 100%/60%/30% -> effects 7/8/9
// MUST only be called from the UI task — see the note on result_queue above.
static void trigger_vibration(char pattern, char intensity)
{
    static const uint8_t click_effects[3]    = {1, 2, 3};
    static const uint8_t buzz_effects[3]     = {47, 49, 51};
    static const uint8_t softbump_effects[3] = {7, 8, 9};

    int idx = (intensity == '1') ? 0 : (intensity == '2') ? 1 : 2; // قوي/متوسط/خفيف
    uint8_t effect;
    switch (pattern) {
        case '1': effect = click_effects[idx]; break;
        case '2': effect = buzz_effects[idx]; break;
        case '3': effect = softbump_effects[idx]; break;
        default: return;
    }
    instance.setHapticEffects(effect);
    instance.vibrator();
}

// Called when a full '#'+category+pattern+intensity sequence arrives (real,
// from the phone over Wi-Fi, on the network task; or simulated, typed into
// Serial on the UI task for testing) — safe to call from either, since it
// only ever queues a message rather than acting on it directly.
static void handle_result_code(char code, char pattern, char intensity, const char *source)
{
    for (size_t i = 0; i < RESULT_CODE_COUNT; i++) {
        if (result_codes[i].code != code) continue;

        Serial.printf("Result (%s): '%c' -> %s (pattern=%c, intensity=%c)\n",
                      source, code, alert_categories[result_codes[i].category_index].label, pattern, intensity);
        ResultMessage msg = {result_codes[i].category_index, pattern, intensity};
        xQueueSend(result_queue, &msg, 0);
        return;
    }
    Serial.printf("Result (%s): unknown category code '%c'\n", source, code);
}

static void build_settings_screen()
{
    settings_screen = make_screen();

    // header row: back chevron + title, fixed height so spacing below is predictable
    lv_obj_t *header = lv_obj_create(settings_screen);
    lv_obj_remove_style_all(header);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(header, lv_pct(100), 28);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *back_btn = lv_button_create(header);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_set_style_pad_all(back_btn, 0, 0);
    lv_obj_set_size(back_btn, 28, 28);
    lv_obj_align(back_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(back_btn, go_to_home_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_icon = lv_label_create(back_btn);
    lv_label_set_text(back_icon, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(back_icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(back_icon, COLOR_TEXT, 0);
    lv_obj_center(back_icon);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "الإعدادات");
    lv_obj_set_style_text_font(title, &tajawal_bold_24, 0);
    lv_obj_set_style_text_color(title, COLOR_TEXT, 0);
    lv_obj_align_to(title, back_btn, LV_ALIGN_OUT_LEFT_MID, -6, 0);

    // everything below the header sits in one flex column with consistent gaps
    lv_obj_t *content = lv_obj_create(settings_screen);
    lv_obj_remove_style_all(content);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(content, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_align_to(content, header, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 10, 0);

    // section label
    lv_obj_t *section = lv_label_create(content);
    lv_label_set_text(section, "طريقة عرض الصوت");
    lv_obj_set_style_text_font(section, &tajawal_regular_16, 0);
    lv_obj_set_style_text_color(section, COLOR_MUTED, 0);
    lv_obj_set_width(section, lv_pct(100));
    lv_obj_set_style_text_align(section, LV_TEXT_ALIGN_RIGHT, 0);

    // segmented toggle: نص / أيقونة  |  لغة الإشارة
    lv_obj_t *toggle = lv_obj_create(content);
    lv_obj_remove_style_all(toggle);
    lv_obj_remove_flag(toggle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(toggle, lv_pct(100), 40);
    lv_obj_set_style_bg_color(toggle, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(toggle, LV_OPA_10, 0);
    lv_obj_set_style_radius(toggle, 10, 0);
    lv_obj_set_style_pad_all(toggle, 3, 0);
    lv_obj_set_flex_flow(toggle, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(toggle, 3, 0);

    seg_text_btn = lv_button_create(toggle);
    lv_obj_set_flex_grow(seg_text_btn, 1);
    lv_obj_set_height(seg_text_btn, lv_pct(100));
    lv_obj_set_style_bg_color(seg_text_btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(seg_text_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(seg_text_btn, 0, 0);
    lv_obj_set_style_radius(seg_text_btn, 8, 0);
    lv_obj_add_event_cb(seg_text_btn, select_text_mode_cb, LV_EVENT_CLICKED, NULL);
    seg_text_lbl_ref = lv_label_create(seg_text_btn);
    lv_label_set_text(seg_text_lbl_ref, "نص / أيقونة");
    lv_obj_set_style_text_font(seg_text_lbl_ref, &tajawal_regular_16, 0);
    lv_obj_set_style_text_color(seg_text_lbl_ref, COLOR_PRIMARY, 0);
    lv_obj_center(seg_text_lbl_ref);

    seg_sign_btn = lv_button_create(toggle);
    lv_obj_set_flex_grow(seg_sign_btn, 1);
    lv_obj_set_height(seg_sign_btn, lv_pct(100));
    lv_obj_set_style_bg_opa(seg_sign_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(seg_sign_btn, 0, 0);
    lv_obj_set_style_radius(seg_sign_btn, 8, 0);
    lv_obj_add_event_cb(seg_sign_btn, select_sign_mode_cb, LV_EVENT_CLICKED, NULL);
    seg_sign_lbl_ref = lv_label_create(seg_sign_btn);
    lv_label_set_text(seg_sign_lbl_ref, "لغة الإشارة");
    lv_obj_set_style_text_font(seg_sign_lbl_ref, &tajawal_regular_16, 0);
    lv_obj_set_style_text_color(seg_sign_lbl_ref, COLOR_TEXT, 0);
    lv_obj_center(seg_sign_lbl_ref);

    if (sign_language_mode) {
        select_segment(seg_sign_btn, seg_sign_lbl_ref, seg_text_btn, seg_text_lbl_ref);
    } else {
        select_segment(seg_text_btn, seg_text_lbl_ref, seg_sign_btn, seg_sign_lbl_ref);
    }

    // action buttons
    lv_obj_t *disconnect_btn = lv_button_create(content);
    lv_obj_set_size(disconnect_btn, lv_pct(100), 36);
    lv_obj_set_style_bg_opa(disconnect_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(disconnect_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(disconnect_btn, LV_OPA_30, 0);
    lv_obj_set_style_border_width(disconnect_btn, 1, 0);
    lv_obj_set_style_radius(disconnect_btn, 10, 0);
    lv_obj_set_style_margin_top(disconnect_btn, 6, 0);
    lv_obj_add_event_cb(disconnect_btn, show_disconnect_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *disconnect_lbl = lv_label_create(disconnect_btn);
    lv_label_set_text(disconnect_lbl, "فصل الساعة عن الجوال");
    lv_obj_set_style_text_font(disconnect_lbl, &tajawal_regular_16, 0);
    lv_obj_set_style_text_color(disconnect_lbl, COLOR_TEXT, 0);
    lv_obj_center(disconnect_lbl);

    lv_obj_t *restart_btn = lv_button_create(content);
    lv_obj_set_size(restart_btn, lv_pct(100), 36);
    lv_obj_set_style_bg_opa(restart_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(restart_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(restart_btn, LV_OPA_30, 0);
    lv_obj_set_style_border_width(restart_btn, 1, 0);
    lv_obj_set_style_radius(restart_btn, 10, 0);
    lv_obj_add_event_cb(restart_btn, restart_watch_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *restart_lbl = lv_label_create(restart_btn);
    lv_label_set_text(restart_lbl, "إعادة تشغيل الساعة");
    lv_obj_set_style_text_font(restart_lbl, &tajawal_regular_16, 0);
    lv_obj_set_style_text_color(restart_lbl, COLOR_TEXT, 0);
    lv_obj_center(restart_lbl);

    lv_obj_t *change_wifi_btn = lv_button_create(content);
    lv_obj_set_size(change_wifi_btn, lv_pct(100), 36);
    lv_obj_set_style_bg_opa(change_wifi_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(change_wifi_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(change_wifi_btn, LV_OPA_30, 0);
    lv_obj_set_style_border_width(change_wifi_btn, 1, 0);
    lv_obj_set_style_radius(change_wifi_btn, 10, 0);
    lv_obj_add_event_cb(change_wifi_btn, change_wifi_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *change_wifi_lbl = lv_label_create(change_wifi_btn);
    lv_label_set_text(change_wifi_lbl, "تغيير شبكة الواي فاي");
    lv_obj_set_style_text_font(change_wifi_lbl, &tajawal_regular_16, 0);
    lv_obj_set_style_text_color(change_wifi_lbl, COLOR_TEXT, 0);
    lv_obj_center(change_wifi_lbl);
}

// --- Wi-Fi / TCP channel: mic audio out, control + result bytes in ---------
//
// This whole section runs on its own FreeRTOS task pinned to the ESP32-S3's
// second core (core 0), separate from loop()/lv_timer_handler() (core 1).
//
// Why: everything used to run in one loop() on one core — UI rendering, mic
// reads, and Wi-Fi/TCP I/O all serialized together. Under real load (audio
// streaming plus Wi-Fi traffic while the phone was busy waiting on the
// classification API) that was enough contention to stall the UI for
// seconds at a time — it looked exactly like a frozen screen, because the
// screen genuinely was waiting its turn behind blocked network calls. Two
// separate tasks on two separate cores means a stall on one side literally
// cannot block the other. handle_result_code() (above) only ever hands data
// to the UI task through result_queue — never touches LVGL directly, since
// LVGL objects must only ever be touched from the one task that also calls
// lv_timer_handler().

// Fallback only, used the very first time the watch boots with nothing
// saved yet in NVS — real credentials come from the app over BLE (see
// start_ble_provisioning() below) and are persisted from then on.
static const char *WIFI_SSID_FALLBACK = "OWAIS_4G";
static const char *WIFI_PASSWORD_FALLBACK = "0530331339";

#define BLE_PROVISIONING_DEVICE_NAME "Nabeeh-Watch-Setup"
#define BLE_WIFI_SERVICE_UUID        "b19c1e70-1fc7-4b2b-9f5b-8f2e6f2b1a01"
#define BLE_WIFI_CHAR_UUID           "b19c1e71-1fc7-4b2b-9f5b-8f2e6f2b1a01"
#define BLE_STATUS_CHAR_UUID         "b19c1e72-1fc7-4b2b-9f5b-8f2e6f2b1a01" // notify-only: 'K' = connected, 'F' = failed/timed out

static BLECharacteristic *status_characteristic = nullptr;

static Preferences wifi_prefs;
static char pending_wifi_ssid[64] = "";
static char pending_wifi_pass[64] = "";
static volatile bool wifi_credentials_pending = false; // set by the BLE callback (its own task), consumed by network_task

// BLECharacteristicCallbacks::onWrite() runs on the Bluetooth stack's own
// task — it only ever parses the incoming value and sets a flag, the same
// hand-off pattern used everywhere else in this file to keep hardware
// access confined to the task that owns it (see result_queue's comment for
// why that matters here).
class WifiProvisionCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *characteristic) override
    {
        String value = characteristic->getValue();
        int sep = value.indexOf('\n'); // expected format: "SSID\nPASSWORD"
        if (sep < 0) {
            Serial.println("BLE Wi-Fi provisioning: malformed value (expected SSID\\nPASSWORD), ignoring.");
            return;
        }
        String ssid = value.substring(0, sep);
        String pass = value.substring(sep + 1);
        if (ssid.length() == 0 || ssid.length() >= sizeof(pending_wifi_ssid) || pass.length() >= sizeof(pending_wifi_pass)) {
            Serial.println("BLE Wi-Fi provisioning: SSID missing or SSID/password too long, ignoring.");
            return;
        }
        strncpy(pending_wifi_ssid, ssid.c_str(), sizeof(pending_wifi_ssid) - 1);
        pending_wifi_ssid[sizeof(pending_wifi_ssid) - 1] = '\0';
        strncpy(pending_wifi_pass, pass.c_str(), sizeof(pending_wifi_pass) - 1);
        pending_wifi_pass[sizeof(pending_wifi_pass) - 1] = '\0';
        wifi_credentials_pending = true;
        Serial.printf("BLE Wi-Fi provisioning: received new SSID '%s'\n", pending_wifi_ssid);
    }
};

// Only advertises while actually needed (first boot before Wi-Fi connects,
// or a user-requested re-provision from Settings) — NOT continuously. Kept
// running all the time, it turned out to fight the display for the radio
// hardware often enough to visibly corrupt screen transitions at random,
// not just the one right after boot. Settings' "تغيير شبكة الواي فاي"
// button (see build_settings_screen()) is the only way back into this mode
// once Wi-Fi is already connected.
//
// The underlying BLE stack/server/service/characteristics are set up once,
// ever (see ensure_ble_initialized()), and never torn down again — "stop"
// only stops advertising. BLEDevice::deinit() used to be called here on
// every stop, which crashed the whole board (Guru Meditation /
// LoadProhibited) if a client was still connected: it disconnects a moment
// later regardless, and by then the stack's already-freed objects were
// still on its way to process that disconnect. Never freeing them avoids
// the race entirely, at the cost of BLE's RAM staying reserved after the
// first use — comfortably affordable given the flash/RAM headroom here.
static bool ble_initialized = false;
static bool ble_active = false;
static volatile bool ble_restart_requested = false; // set on the UI task (Settings button), consumed on the network task
static BLEAdvertising *ble_advertising = nullptr;

static void ensure_ble_initialized()
{
    if (ble_initialized) return;
    BLEDevice::init(BLE_PROVISIONING_DEVICE_NAME);
    BLEServer *server = BLEDevice::createServer();
    BLEService *service = server->createService(BLE_WIFI_SERVICE_UUID);
    BLECharacteristic *characteristic = service->createCharacteristic(
        BLE_WIFI_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE);
    characteristic->setCallbacks(new WifiProvisionCallbacks());

    // App has no other way to know whether the credentials it just wrote
    // actually worked — notify 'K' (connected) or 'F' (failed/timed out)
    // once network_task's loop resolves the attempt (see there). Clients
    // must subscribe (write to the 0x2902 descriptor) before notifications
    // arrive — standard BLE, most libraries (e.g. flutter_blue_plus) do
    // this automatically when you call their "subscribe"/"setNotifyValue".
    status_characteristic = service->createCharacteristic(
        BLE_STATUS_CHAR_UUID, BLECharacteristic::PROPERTY_NOTIFY);
    status_characteristic->addDescriptor(new BLE2902());

    service->start();
    ble_advertising = server->getAdvertising();
    ble_initialized = true;
}

static void start_ble_provisioning()
{
    if (ble_active) return;
    ensure_ble_initialized();
    ble_advertising->start();
    ble_active = true;
    Serial.printf("BLE Wi-Fi provisioning advertising as '%s'.\n", BLE_PROVISIONING_DEVICE_NAME);
}

static void stop_ble_provisioning()
{
    if (!ble_active) return;
    ble_advertising->stop();
    ble_active = false;
    Serial.println("BLE Wi-Fi provisioning stopped (Wi-Fi connected).");
}

// Settings screen button: only sets a flag here (UI task) — network_task
// picks it up and actually starts BLE on its own task, same hand-off
// pattern as everything else that touches shared hardware in this file.
static void change_wifi_cb(lv_event_t *e)
{
    ble_restart_requested = true;
}

// NVS-persisted (survives reboots/reflashes) — falls back to the hardcoded
// dev credentials above only if nothing has ever been saved.
static void load_wifi_credentials(char *ssid_out, size_t ssid_len, char *pass_out, size_t pass_len)
{
    wifi_prefs.begin("nabeeh", true);
    String saved_ssid = wifi_prefs.getString("wifi_ssid", "");
    String saved_pass = wifi_prefs.getString("wifi_pass", "");
    wifi_prefs.end();
    if (saved_ssid.length() > 0) {
        strncpy(ssid_out, saved_ssid.c_str(), ssid_len - 1);
        ssid_out[ssid_len - 1] = '\0';
        strncpy(pass_out, saved_pass.c_str(), pass_len - 1);
        pass_out[pass_len - 1] = '\0';
    } else {
        strncpy(ssid_out, WIFI_SSID_FALLBACK, ssid_len - 1);
        ssid_out[ssid_len - 1] = '\0';
        strncpy(pass_out, WIFI_PASSWORD_FALLBACK, pass_len - 1);
        pass_out[pass_len - 1] = '\0';
    }
}

static void save_wifi_credentials(const char *ssid, const char *pass)
{
    wifi_prefs.begin("nabeeh", false);
    wifi_prefs.putString("wifi_ssid", ssid);
    wifi_prefs.putString("wifi_pass", pass);
    wifi_prefs.end();
}
static const uint16_t TCP_PORT = 3333;
static const size_t CHUNK_SIZE = 1024; // 512 samples @ 16-bit = ~32ms of audio per chunk

WiFiServer server(TCP_PORT);
WiFiClient client;
static bool was_connected = false;
static volatile bool streaming = false; // written on both tasks (Serial on UI task, client on network task), read on network task; a plain flag is fine for this single on/off signal

// '#' now starts a 3-byte sequence: category, then vibration pattern digit
// ('1'/'2'/'3'), then vibration intensity digit ('1'/'2'/'3') — e.g. "#B11"
// = baby crying, pattern 1 (click), intensity 1 (قوي/100%). All measured
// from the initial '#' — a real app sends the whole 4-byte message in one
// write, so budgeting from the start (rather than resetting per-byte) is
// simpler and still generous for that case.
enum ResultParseState { RESULT_IDLE, RESULT_AWAIT_CATEGORY, RESULT_AWAIT_PATTERN, RESULT_AWAIT_INTENSITY };
static ResultParseState result_parse_state = RESULT_IDLE;
static char pending_category = 0;
static char pending_pattern = 0;
static unsigned long result_parse_started = 0; // millis() when the '#' arrived, so a stray/interrupted sequence can't wedge the parser forever
#define RESULT_CODE_WAIT_TIMEOUT_MS 1000
#define WIFI_RECONNECT_TIMEOUT_MS 15000
#define RECONNECT_BACKOFF_MAX_MS 30000
static uint8_t audio_buf[CHUNK_SIZE];

// "last sync" = how long the *current* connection has been up (counts up
// while connected); once disconnected, it switches to how long ago that
// connection ended instead. phone_connected itself is the watch's own idea
// of pairing state, set only from the Settings screen's disconnect button
// (UI task) — reported here as-is, not touched from this task, to keep
// every LVGL-adjacent variable's writes on the one task that owns the UI.
static unsigned long connection_start_millis = 0;
static unsigned long last_disconnect_millis = 0;
static bool ever_connected = false;

static bool is_valid_result_code(char c)
{
    for (size_t i = 0; i < RESULT_CODE_COUNT; i++) {
        if (result_codes[i].code == c) return true;
    }
    return false;
}

static void handle_incoming_byte(char c, const char *source)
{
    if (result_parse_state != RESULT_IDLE) {
        bool timed_out = millis() - result_parse_started > RESULT_CODE_WAIT_TIMEOUT_MS;
        bool valid = (result_parse_state == RESULT_AWAIT_CATEGORY) ? is_valid_result_code(c) : (c == '1' || c == '2' || c == '3');
        // Only actually consume this byte as the next step of the '#...'
        // sequence if it both arrived in time AND looks like what's
        // expected at this stage. Otherwise abandon the sequence and let
        // this byte fall through to be handled as a normal, independent
        // byte below — a stray or malformed '#...' used to silently
        // swallow whatever came right after it, even a completely
        // unrelated command like 'i', with no response ever sent and
        // nothing in the log to explain why.
        if (!timed_out && valid) {
            if (result_parse_state == RESULT_AWAIT_CATEGORY) {
                pending_category = c;
                result_parse_state = RESULT_AWAIT_PATTERN;
            } else if (result_parse_state == RESULT_AWAIT_PATTERN) {
                pending_pattern = c;
                result_parse_state = RESULT_AWAIT_INTENSITY;
            } else {
                handle_result_code(pending_category, pending_pattern, c, source);
                result_parse_state = RESULT_IDLE;
            }
            return;
        }
        Serial.printf("Result (%s): '#...' sequence interrupted by unexpected byte '%c'%s — abandoning it, processing '%c' normally.\n",
                       source, c, timed_out ? " (timed out)" : "", c);
        result_parse_state = RESULT_IDLE;
    }
    if (c == '#') {
        result_parse_state = RESULT_AWAIT_CATEGORY;
        result_parse_started = millis();
    } else if (c == 'r' || c == 'R') {
        streaming = true;
        Serial.printf("Streaming started (%s).\n", source);
    } else if (c == 's' || c == 'S') {
        streaming = false;
        Serial.printf("Streaming stopped (%s).\n", source);
    } else if (c == 'i' || c == 'I') {
        int battery = instance.pmu.getBatteryPercent(); // -1 if unavailable (e.g. no battery connected)
        unsigned long sync_ago_sec;
        if (client && client.connected()) {
            sync_ago_sec = (millis() - connection_start_millis) / 1000; // how long this session has been up
        } else if (ever_connected) {
            sync_ago_sec = (millis() - last_disconnect_millis) / 1000; // how long since contact was lost
        } else {
            sync_ago_sec = 0; // never connected at all yet
        }
        char resp[48];
        snprintf(resp, sizeof(resp), "I,%d,%d,%lu\n", phone_connected ? 1 : 0, battery, sync_ago_sec);
        Serial.printf("Info request (%s): %s", source, resp);
        if (strcmp(source, "client") == 0 && client && client.connected()) {
            client.print(resp);
        }
    }
}

static void network_task(void *pvParameters)
{
    char wifi_ssid[64], wifi_pass[64];
    load_wifi_credentials(wifi_ssid, sizeof(wifi_ssid), wifi_pass, sizeof(wifi_pass));

    // BLE being active AT ALL turned out to risk corrupting whatever screen
    // transition happens to land while it's on — not just the one right
    // after boot, ANY of them, since the user can tap through screens at
    // any moment. Delaying BLE's start couldn't fully avoid this (there's
    // no way to know when the user will tap next), so instead: don't turn
    // BLE on at all unless it's actually needed. Give the saved/fallback
    // credentials a real chance to connect first — that's the common case
    // on every boot after the first — and only fall back to BLE
    // provisioning if they fail. It's stopped again below the moment Wi-Fi
    // connects either way (see the loop), so its active window is as short
    // as possible whenever it does have to run.
    Serial.printf("Connecting to Wi-Fi network: %s\n", wifi_ssid);
    WiFi.mode(WIFI_STA);
    // Arduino-ESP32's built-in auto-reconnect retries roughly every 2s with
    // no backoff — fine normally, but bad credentials (typo'd during a
    // reprovision, say) turn that into a near-continuous retry loop, which
    // shares the radio with BLE closely enough to make BLE unreliable right
    // when it's needed most (to fix the bad credentials). Handling
    // reconnects manually below with real backoff avoids that.
    WiFi.setAutoReconnect(false);
    WiFi.begin(wifi_ssid, wifi_pass);
    unsigned long connect_start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - connect_start < 10000) {
        vTaskDelay(pdMS_TO_TICKS(300));
        Serial.print(".");
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println();
        Serial.println("Could not connect with saved/fallback credentials — starting BLE provisioning.");
        start_ble_provisioning();
        while (WiFi.status() != WL_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(300));
            Serial.print(".");
            if (wifi_credentials_pending) break; // new credentials arrived over BLE — stop retrying the old ones
        }
        if (WiFi.status() == WL_CONNECTED) {
            stop_ble_provisioning(); // the original credentials worked after all, just slower than 10s
        }
    }
    Serial.println();
    Serial.println("Wi-Fi connected.");
    Serial.print("Watch IP address: ");
    Serial.println(WiFi.localIP());

    server.begin();
    Serial.printf("TCP server listening on port %u\n", TCP_PORT);
    Serial.println("Send 'r'/'s' to start/stop mic streaming, or '#'+code (e.g. '#B') to simulate a result.");

    bool wifi_reconnect_pending = false; // true from the moment new BLE-provisioned credentials are applied until we know whether they worked
    unsigned long wifi_reconnect_started = 0;

    // General-purpose reconnect with backoff, for any other disconnect
    // (router hiccup, out of range, etc.) — separate from wifi_reconnect_pending
    // above, which is specifically the one-shot 15s-then-notify attempt
    // right after new BLE-provisioned credentials.
    unsigned long last_reconnect_attempt = millis();
    unsigned long reconnect_backoff_ms = 2000;

    for (;;) {
        // Skip entirely while BLE is actively open: that already means Wi-Fi
        // isn't working and someone may be mid-way through fixing it over
        // BLE right now — competing background reconnect attempts (even
        // backed off) are exactly what made BLE unreliable last time.
        if (!wifi_reconnect_pending && !ble_active) {
            if (WiFi.status() == WL_CONNECTED) {
                reconnect_backoff_ms = 2000; // reset once healthy
            } else if (millis() - last_reconnect_attempt >= reconnect_backoff_ms) {
                last_reconnect_attempt = millis();
                Serial.printf("Wi-Fi disconnected — retrying (next backoff %lums)...\n", reconnect_backoff_ms);
                WiFi.begin(wifi_ssid, wifi_pass);
                reconnect_backoff_ms = (reconnect_backoff_ms * 2 > RECONNECT_BACKOFF_MAX_MS) ? RECONNECT_BACKOFF_MAX_MS : reconnect_backoff_ms * 2;
            }
        }

        if (wifi_reconnect_pending) {
            if (WiFi.status() == WL_CONNECTED) {
                wifi_reconnect_pending = false;
                Serial.println("Reprovisioned Wi-Fi connected.");
                if (status_characteristic) {
                    status_characteristic->setValue("K");
                    status_characteristic->notify();
                }
                stop_ble_provisioning(); // only stops advertising now — safe even with a client still connected, see its own comment
            } else if (millis() - wifi_reconnect_started > WIFI_RECONNECT_TIMEOUT_MS) {
                wifi_reconnect_pending = false;
                Serial.println("Reprovisioned Wi-Fi failed to connect within 15s.");
                if (status_characteristic) {
                    status_characteristic->setValue("F");
                    status_characteristic->notify();
                }
                // Deliberately leave BLE running on failure so the user can
                // just try again with different credentials, instead of
                // having to reopen provisioning from Settings first.
            }
        }

        if (ble_restart_requested) {
            ble_restart_requested = false;
            start_ble_provisioning();
        }

        if (wifi_credentials_pending) {
            wifi_credentials_pending = false;
            strncpy(wifi_ssid, pending_wifi_ssid, sizeof(wifi_ssid) - 1);
            wifi_ssid[sizeof(wifi_ssid) - 1] = '\0';
            strncpy(wifi_pass, pending_wifi_pass, sizeof(wifi_pass) - 1);
            wifi_pass[sizeof(wifi_pass) - 1] = '\0';
            save_wifi_credentials(wifi_ssid, wifi_pass);
            Serial.printf("Reconnecting to newly provisioned Wi-Fi network: %s\n", wifi_ssid);
            WiFi.disconnect();
            WiFi.begin(wifi_ssid, wifi_pass);
            wifi_reconnect_pending = true;
            wifi_reconnect_started = millis();
        }

        if (!client || !client.connected()) {
            if (was_connected) {
                Serial.println("Client disconnected.");
                was_connected = false;
                streaming = false;
                last_disconnect_millis = millis();
                ever_connected = true;
            }
            WiFiClient newClient = server.available();
            if (newClient) {
                client = newClient;
                client.setNoDelay(true); // send audio chunks immediately, don't batch
                was_connected = true;
                connection_start_millis = millis();
                result_parse_state = RESULT_IDLE; // a previous connection can't leave half-read state behind
                Serial.println("Client connected.");
            }
        }

        if (client && client.connected() && client.available()) {
            handle_incoming_byte(client.read(), "client");
        }

        // Mic reads block for ~32ms per chunk while streaming, and write()
        // can block for up to its 3s timeout if the phone stops draining the
        // socket — both fine here, since this task has nothing time-critical
        // to do and, being on its own core, can never stall the UI task even
        // if it blocks. (A previous version tried to guard the write with
        // availableForWrite(), but NetworkClient never overrides that — it
        // always inherits Print's default of 0 — so the guard silently
        // dropped every chunk, unconditionally. Not doing that again.)
        if (streaming && client && client.connected()) {
            size_t n = instance.mic.readBytes((char *)audio_buf, CHUNK_SIZE);
            if (n > 0) {
                client.write(audio_buf, n);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void setup()
{
    Serial.begin(115200);
    delay(300);

    instance.begin(); // also sets up instance.mic: PDM, 16kHz, mono, 16-bit
    beginLvglHelper(instance);

    build_splash_screen();
    build_onboarding_screen();
    build_home_screen();
    lv_screen_load(splash_screen);

    // PCF8563 keeps time across reboots on its own backup battery. Used to
    // force-reseed it from __DATE__/__TIME__ on every boot, but that macro
    // only changes when the file's *content* changes — a content-hash-based
    // incremental build can (and did) reuse a cached object file compiled on
    // an earlier day, silently keeping a stale embedded date across many
    // later reflashes. Only seed it if it looks uninitialized; once set
    // correctly here, trust the RTC's own battery-backed clock going
    // forward (until there's a phone to sync real time from instead).
    if (instance.rtc.getDateTime().getYear() < 2024) {
        instance.rtc.setDateTime(RTC_DateTime(2026, 8, 16, 9, 16, 15)); // Asia/Riyadh, UTC+3
    }
    update_clock_display_cb(NULL);
    lv_timer_create(update_clock_display_cb, 30000, NULL);
    update_connection_status();

    instance.setBrightness(DEVICE_MAX_BRIGHTNESS_LEVEL);

    result_queue = xQueueCreate(4, sizeof(ResultMessage));
    // Core 1 runs the Arduino loop()/LVGL; pin the network/audio task to the
    // other core (0) so it can never contend with UI rendering for CPU time.
    xTaskCreatePinnedToCore(network_task, "network", 16384, NULL, 1, NULL, 0);
}

void loop()
{
    lv_timer_handler();

    ResultMessage msg;
    if (xQueueReceive(result_queue, &msg, 0) == pdTRUE) {
        show_result_category(msg.category_index);
        trigger_vibration(msg.pattern, msg.intensity); // I2C on the same bus as RTC/PMU — UI task only, see result_queue's comment
    }

    if (Serial.available()) {
        handle_incoming_byte(Serial.read(), "Serial");
    }

    delay(2);
}
