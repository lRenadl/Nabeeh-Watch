/**
 * Nabeeh watch UI — Home (clock), Settings, Alert, and sign-language screens.
 * Arabic text uses IBM Plex Sans Arabic (symbol names still say "tajawal_*"
 * for historical reasons, not worth a mechanical rename right now):
 * tajawal_bold_24 (titles) and tajawal_regular_16 (everything else).
 * Latin digits (clock, battery %) use the built-in Montserrat fonts.
 */
#include <LilyGoLib.h>
#include <LV_Helper.h>
#include <string.h>
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
static void arabic_digits(int n, char *out)
{
    static const char *digit[10] = {"٠", "١", "٢", "٣", "٤", "٥", "٦", "٧", "٨", "٩"};
    out[0] = '\0';
    if (n >= 10) strcat(out, digit[n / 10]);
    strcat(out, digit[n % 10]);
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
static void demo_trigger_alert_cb(lv_event_t *e);

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
    // TEMPORARY: tap to preview the alert screen without a phone connection
    lv_obj_add_event_cb(status_row, demo_trigger_alert_cb, LV_EVENT_CLICKED, NULL);
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

// TEMPORARY test hook: tapping the "متصلة" status on Home cycles through the
// 5 categories so the alert screen can be reviewed without a phone connection.
static int demo_alert_index = 0;
static void demo_trigger_alert_cb(lv_event_t *e)
{
    show_alert(demo_alert_index);
    demo_alert_index = (demo_alert_index + 1) % ALERT_CATEGORY_COUNT;
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
}

void setup()
{
    Serial.begin(115200);

    instance.begin();
    beginLvglHelper(instance);

    build_splash_screen();
    build_onboarding_screen();
    build_home_screen();
    lv_screen_load(splash_screen);

    // PCF8563 keeps time across reboots on its own backup battery, but it
    // already had a stale factory-set date on it — until there's a phone to
    // sync real time from, just re-seed it from the build machine's clock
    // (Asia/Riyadh, UTC+3) on every boot.
    instance.rtc.setDateTime(RTC_DateTime(__DATE__, __TIME__));
    update_clock_display_cb(NULL);
    lv_timer_create(update_clock_display_cb, 30000, NULL);
    update_connection_status();

    instance.setBrightness(DEVICE_MAX_BRIGHTNESS_LEVEL);
}

void loop()
{
    lv_timer_handler();
    delay(2);
}
