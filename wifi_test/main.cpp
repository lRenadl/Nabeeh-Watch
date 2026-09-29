/**
 * Nabeeh watch — full integration: the LVGL UI screens (originally
 * main_ui_backup.cpp) plus the Wi-Fi/TCP channel (mic audio out, control
 * and now classification results in).
 *
 * Audio format: 16 kHz, 16-bit, mono PCM — the mic's native format, so no
 * resampling is needed. ~32 KB/s, trivial for local Wi-Fi TCP.
 *
 * Wire protocol (one TCP connection, port 3333, full-duplex):
 *   Connection handshake — required before ANY command below is accepted:
 *   Pairing (only while unpaired: first boot, or after "إلغاء ارتباط الهاتف"):
 *     watch -> phone : "PAIR,<64 hex>\n"  watch's ephemeral X25519 public key
 *     phone -> watch : "PAIR,<64 hex>\n"  phone's ephemeral X25519 public key
 *                      both derive key = SHA-256("nabeeh-pair-v1" || X25519
 *                      shared secret || watch pub || phone pub) — the key
 *                      itself never goes over the network. Then the normal
 *                      AUTH exchange below proves both got the same key, and
 *                      the watch only saves it once that succeeds.
 *   Every connection (and right after pairing):
 *     watch -> phone : "AUTH,<32 hex>\n"  16-byte random nonce
 *     phone -> watch : "AUTH,<64 hex>\n"  HMAC-SHA256(device key, nonce bytes)
 *     watch -> phone : "AUTH,OK\n" on success, or "AUTH,FAIL\n" then close on
 *                      a wrong key. Malformed/late (>5s) responses are just
 *                      closed — only AUTH,FAIL means the saved key is bad.
 *   watch -> phone : raw PCM audio bytes, continuously, while streaming
 *   phone -> watch : single control bytes
 *     'r' / 'R'  -> start streaming mic audio               (unchanged)
 *     's' / 'S'  -> stop streaming mic audio                (unchanged)
 *     '#' + code -> classification result OR a phone reminder firing;
 *                   show it on the watch (see result_codes[] below for the
 *                   category letters — 'T' is reminders specifically, the
 *                   rest are sound classification results). '#' was picked
 *                   instead of reusing 'R' for this because 'R' already
 *                   means "start streaming" above and reusing it would
 *                   collide. Reminders only support vibration
 *                   pattern/intensity digits '1'-'3' like everything else
 *                   here — the app's 4th reminder-only pattern option
 *                   ("تصاعدي") gets clamped down to '3' before it's sent,
 *                   since this firmware has no 4th pattern to show it as.
 *     '@' + list + '\n'
 *                -> the phone's full reminder schedule. The watch stores it
 *                   in NVS and fires each reminder from its own RTC, so a
 *                   reminder still vibrates when the phone is asleep, out of
 *                   Wi-Fi range, or dead. Entries are ';'-separated, fields
 *                   ':'-separated:
 *                       H:M:daysMask:pattern:intensity:once:label
 *                   `label` is the reminder's own name, shown on the watch
 *                   instead of the generic "تذكير". It is the last field so
 *                   it may safely contain ':' — only ';' and newlines are
 *                   reserved, and the phone strips those. It is optional: a
 *                   schedule saved by an older build still loads.
 *                   daysMask bit0=Monday .. bit6=Sunday (same 1..7 numbering
 *                   as Dart's DateTime.weekday), once=1 means the entry is
 *                   dropped after it fires. '@\n' alone clears the schedule.
 *                   Replies "R,<count>\n" so the phone can confirm.
 *                   The phone ALSO still sends '#T..' at the moment a
 *                   reminder fires if it happens to be awake — whichever
 *                   arrives first wins, the other is suppressed (see
 *                   REMINDER_DUPLICATE_WINDOW_MS).
 *     '?'        -> dump the full reminder state to Serial (stored schedule,
 *                   RTC time, computed weekday, per-entry flags). The one
 *                   command to run when a reminder didn't fire.
 *     '!'        -> fire a test reminder immediately, ignoring the clock and
 *                   the duplicate-suppression window.
 * Same commands work over USB Serial too (dev/testing convenience) — e.g.
 * typing "#B" in the Serial Monitor simulates a "باكاء أطفال" result
 * without needing the phone to send anything for real.
 *
 * Build/upload with: pio run -e wifi_test -t upload -t monitor
 */
#include <LilyGoLib.h>
#include <LV_Helper.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <esp_random.h>
#include <mbedtls/md.h>     // HMAC-SHA256 for TCP connection authentication — see compute_hmac_sha256()
#include <mbedtls/ecdh.h>   // X25519 key agreement for pairing — see pairing_begin()/pairing_finish()
#include <mbedtls/sha256.h>
#include <lwip/sockets.h>   // select() — non-blocking audio send, see socket_writable_now()
#include <time.h>
#include <sys/time.h> // settimeofday() — used to invalidate the RTC-seeded system clock before an NTP sync
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
    // لا يوجد أيقونة أو فيديو لغة إشارة مخصص للتذكيرات بعد — نعيد استخدام
    // أيقونة الجرس (أقرب شكل بصري لتنبيه/منبّه من الأيقونات الجاهزة) و
    // sign_gif=nullptr، مع تعديل show_sign_result أدناه ليتجاهل وضع لغة
    // الإشارة ويرجع لعرض الأيقونة+النص العادي كلما كان sign_gif فاضي.
    {&emoji_bell,   nullptr,        "تذكير"},
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
    {'T', 5}, // تذكير — من ميزة المنبّهات بالتطبيق، مو من تصنيف صوت
};
#define RESULT_CODE_COUNT (sizeof(result_codes) / sizeof(result_codes[0]))

// ── جدول تذكيرات محفوظ داخل الساعة ────────────────────────────────────────
// الساعة تخزّن مواعيد التذكيرات وتطلقها من ساعتها الداخلية (RTC)، فما تعتمد
// على كون الجوال صاحيًا ولا على نفس شبكة الواي فاي وقت الموعد — وهذا مهم
// لمستخدم أصم: السوار هو قناة التنبيه الأساسية، ما يصح تعتمد على جهاز ثاني.
// الجوال يرسل الجدول كامل كل ما يتغيّر تذكير (وكل ما يتصل بالساعة)، والساعة
// تحفظه بـNVS فيبقى بعد إعادة التشغيل.
#define MAX_WATCH_REMINDERS 16
// نافذة كتم التكرار: نفس التذكير ممكن يوصل مرتين — من ساعة الساعة نفسها، ومن
// الجوال لو كان صاحيًا وقتها. أول واحد يوصل يشتغل والثاني يُتجاهل خلال هذي
// النافذة. أقل من دقيقة عن قصد، عشان تذكيرين بدقيقتين متتاليتين يشتغلون عادي.
#define REMINDER_DUPLICATE_WINDOW_MS 45000UL

struct WatchReminder {
    uint8_t hour;
    uint8_t minute;
    uint8_t days_mask; // bit0=الاثنين .. bit6=الأحد
    char pattern;      // '1'-'3' — نفس أرقام trigger_vibration
    char intensity;    // '1'-'3'
    bool once;         // تذكير غير متكرر: يُشطب بعد ما يشتغل
    bool spent;
    char label[64];    // اسم التذكير كما كتبه المستخدم؛ فاضي = نعرض "تذكير"
};
// فهرس فئة "تذكير" داخل alert_categories، مأخوذ من نفس جدول الأكواد بدل ما
// يُكتب رقمًا ثابتًا هنا — عشان إضافة فئة جديدة فوق ما تكسره بصمت.
static int reminder_category_index()
{
    for (size_t i = 0; i < RESULT_CODE_COUNT; i++) {
        if (result_codes[i].code == 'T') return result_codes[i].category_index;
    }
    return -1;
}

// تشخيص: سطر لكل دقيقة يوضح وقت الساعة واليوم وعدد التذكيرات المخزّنة.
// حطّه 0 بعد ما تستقر الميزة.
#define REMINDER_DEBUG_LOG 1

static WatchReminder watch_reminders[MAX_WATCH_REMINDERS];
static int watch_reminder_count = 0;
static volatile unsigned long last_reminder_dispatch_ms = 0;

// يشيل أي حرف UTF-8 ناقص من نهاية النص. يصير فقط لو وصل اسم أطول من المخزن
// فانقص بالنص — وبايت نصف حرف يطلع مربعًا فاضيًا على الشاشة بدل ما يُتجاهل.
static void trim_incomplete_utf8(char *str)
{
    size_t len = strlen(str);
    size_t i = len;
    while (i > 0 && ((unsigned char)str[i - 1] & 0xC0) == 0x80) i--; // بايتات التكملة
    if (i == 0) return;

    unsigned char lead = (unsigned char)str[i - 1];
    size_t need = (lead < 0x80)             ? 1
                  : ((lead & 0xE0) == 0xC0) ? 2
                  : ((lead & 0xF0) == 0xE0) ? 3
                  : ((lead & 0xF8) == 0xF0) ? 4
                                            : 1;
    if ((i - 1) + need > len) str[i - 1] = '\0'; // الحرف الأخير ناقص
}

// ── شبكات واي فاي محفوظة ───────────────────────────────────────────────────
// الساعة تحفظ عدة شبكات (مو وحدة بس زي قبل) عشان المستخدم يقدر يتنقل بينها
// من شاشة إعدادات الواي فاي بدون ما يعيد كتابة كلمة المرور كل مرة. saved_networks[0]
// دايمًا آخر شبكة اتصلت/اتضافت (الأحدث أول) — هذا الترتيب هو اللي load_wifi_credentials
// تعتمد عليه عند الإقلاع (تجرب آخر شبكة نجحت)، ونفس الدالة add_or_update_saved_network
// تستخدمها كل من: إعداد شبكة جديدة عبر BLE/نقطة الوصول، والضغط على شبكة محفوظة
// من القائمة للاتصال بها — كلاهما "يستخدم" الشبكة فتصير الأحدث تلقائيًا.
//
// wifi_prefs مُعرّفة هنا (مو بقسم الواي فاي/الشبكة تحت) عشان هذي الدوال تحتاجها
// وتُستدعى من شاشات الواجهة المبنية قبل ذاك القسم بالملف.
static Preferences wifi_prefs;
#define MAX_SAVED_NETWORKS 5
struct SavedNetwork {
    char ssid[64];
    char pass[64];
};
static SavedNetwork saved_networks[MAX_SAVED_NETWORKS];
static int saved_network_count = 0;

// يحمّل القائمة من NVS. لو ما فيه قائمة محفوظة بعد (تحديث من نسخة أقدم كانت
// تخزن شبكة وحدة بمفاتيح "wifi_ssid"/"wifi_pass")، يهاجر تلك الشبكة كأول
// عنصر بالقائمة الجديدة بدل ما يفقدها.
static void load_saved_networks()
{
    wifi_prefs.begin("nabeeh", true);
    saved_network_count = wifi_prefs.getUChar("net_count", 0);
    if (saved_network_count > MAX_SAVED_NETWORKS) saved_network_count = MAX_SAVED_NETWORKS;
    for (int i = 0; i < saved_network_count; i++) {
        char key_s[12], key_p[12];
        snprintf(key_s, sizeof(key_s), "net%d_s", i);
        snprintf(key_p, sizeof(key_p), "net%d_p", i);
        String s = wifi_prefs.getString(key_s, "");
        String p = wifi_prefs.getString(key_p, "");
        strncpy(saved_networks[i].ssid, s.c_str(), sizeof(saved_networks[i].ssid) - 1);
        saved_networks[i].ssid[sizeof(saved_networks[i].ssid) - 1] = '\0';
        strncpy(saved_networks[i].pass, p.c_str(), sizeof(saved_networks[i].pass) - 1);
        saved_networks[i].pass[sizeof(saved_networks[i].pass) - 1] = '\0';
    }
    String legacy_ssid = wifi_prefs.getString("wifi_ssid", "");
    String legacy_pass = wifi_prefs.getString("wifi_pass", "");
    wifi_prefs.end();

    if (saved_network_count == 0 && legacy_ssid.length() > 0) {
        strncpy(saved_networks[0].ssid, legacy_ssid.c_str(), sizeof(saved_networks[0].ssid) - 1);
        saved_networks[0].ssid[sizeof(saved_networks[0].ssid) - 1] = '\0';
        strncpy(saved_networks[0].pass, legacy_pass.c_str(), sizeof(saved_networks[0].pass) - 1);
        saved_networks[0].pass[sizeof(saved_networks[0].pass) - 1] = '\0';
        saved_network_count = 1;
        // نحفظها فورًا بالصيغة الجديدة عشان الهجرة تصير مرة وحدة بس، مو كل إقلاع.
        wifi_prefs.begin("nabeeh", false);
        wifi_prefs.putUChar("net_count", 1);
        wifi_prefs.putString("net0_s", saved_networks[0].ssid);
        wifi_prefs.putString("net0_p", saved_networks[0].pass);
        wifi_prefs.end();
    }
}

static void save_saved_networks()
{
    wifi_prefs.begin("nabeeh", false);
    wifi_prefs.putUChar("net_count", (uint8_t)saved_network_count);
    for (int i = 0; i < saved_network_count; i++) {
        char key_s[12], key_p[12];
        snprintf(key_s, sizeof(key_s), "net%d_s", i);
        snprintf(key_p, sizeof(key_p), "net%d_p", i);
        wifi_prefs.putString(key_s, saved_networks[i].ssid);
        wifi_prefs.putString(key_p, saved_networks[i].pass);
    }
    wifi_prefs.end();
}

// يضيف شبكة جديدة أو يحدّث كلمة مرور شبكة موجودة، ويخليها بالمقدمة (index 0)
// دايمًا — هذا هو ترتيب "الأحدث استخدامًا أول". لو القائمة مليانة وهذي شبكة
// جديدة فعلاً، أقدم شبكة (آخر عنصر) تنحذف عشان تفسح لها مكان.
static void add_or_update_saved_network(const char *ssid, const char *pass)
{
    int existing = -1;
    for (int i = 0; i < saved_network_count; i++) {
        if (strcmp(saved_networks[i].ssid, ssid) == 0) {
            existing = i;
            break;
        }
    }
    int shift_from = (existing >= 0) ? existing : ((saved_network_count >= MAX_SAVED_NETWORKS) ? MAX_SAVED_NETWORKS - 1 : saved_network_count);
    for (int i = shift_from; i > 0; i--) {
        saved_networks[i] = saved_networks[i - 1];
    }
    strncpy(saved_networks[0].ssid, ssid, sizeof(saved_networks[0].ssid) - 1);
    saved_networks[0].ssid[sizeof(saved_networks[0].ssid) - 1] = '\0';
    strncpy(saved_networks[0].pass, pass, sizeof(saved_networks[0].pass) - 1);
    saved_networks[0].pass[sizeof(saved_networks[0].pass) - 1] = '\0';
    if (existing < 0 && saved_network_count < MAX_SAVED_NETWORKS) saved_network_count++;
    save_saved_networks();
    Serial.printf("DBG saved_networks after add/update '%s': count=%d ->", ssid, saved_network_count);
    for (int i = 0; i < saved_network_count; i++) Serial.printf(" [%d]=%s", i, saved_networks[i].ssid);
    Serial.println();
}

// يمسح كل الشبكات المحفوظة (زر "مسح الكل" بشاشة إعدادات الواي فاي). يمسح
// مفاتيح الصيغة القديمة كمان عشان الهجرة بـload_saved_networks ما ترجّعها.
static void clear_saved_networks()
{
    wifi_prefs.begin("nabeeh", false);
    wifi_prefs.putUChar("net_count", 0);
    wifi_prefs.remove("wifi_ssid");
    wifi_prefs.remove("wifi_pass");
    wifi_prefs.end();
    saved_network_count = 0;
}

// ── مصادقة اتصال TCP بمفتاح فريد لكل جهاز (HMAC-SHA256) ────────────────────
// المفتاح ما ينتقل على الشبكة أبدًا: وقت الإقران (device_paired == false) الساعة
// والجوال يسوون تبادل مفاتيح X25519 (ECDH) — كل طرف يرسل مفتاحه العام المؤقت
// بس، ويحسب منه نفس السر المشترك، واللي يراقب الشبكة ما يقدر يوصل له. المفتاح
// النهائي = SHA-256("nabeeh-pair-v1" || السر || عام الساعة || عام الجوال).
// ما ينحفظ إلا بعد ما يثبت الجوال إنه وصل لنفس المفتاح (رد AUTH صحيح)، فإقران
// ناقص ما يستهلك نافذة الإقران. بعدها كل اتصال يثبت معرفته بالمفتاح عبر تحدٍّ
// (nonce) — انظر network_task. "إلغاء ارتباط الهاتف" بشاشة الإعدادات
// يلغي المفتاح الحالي ويفتح نافذة إقران جديدة (تغيير الجوال أو فقدان المفتاح).
// حد معروف: مهاجم نشط يعترض الاتصال بلحظة الإقران نفسها (MITM) يقدر يخدع
// الطرفين؛ التنصت وحده ما يكفيه.
#define DEVICE_KEY_LEN 32
static uint8_t device_key[DEVICE_KEY_LEN];
static bool device_paired = false;

// Ephemeral X25519 state for the one pairing handshake in progress (if any).
static mbedtls_ecp_group pair_grp;
static mbedtls_mpi pair_priv;

static int hw_rng(void *, unsigned char *buf, size_t len)
{
    esp_fill_random(buf, len);
    return 0;
}

static bool pairing_begin(uint8_t watch_pub_out[32])
{
    mbedtls_ecp_group_free(&pair_grp);
    mbedtls_mpi_free(&pair_priv);
    mbedtls_ecp_group_init(&pair_grp);
    mbedtls_mpi_init(&pair_priv);
    mbedtls_ecp_point pub;
    mbedtls_ecp_point_init(&pub);
    size_t olen = 0;
    bool ok = mbedtls_ecp_group_load(&pair_grp, MBEDTLS_ECP_DP_CURVE25519) == 0 &&
              mbedtls_ecdh_gen_public(&pair_grp, &pair_priv, &pub, hw_rng, NULL) == 0 &&
              mbedtls_ecp_point_write_binary(&pair_grp, &pub, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                             &olen, watch_pub_out, 32) == 0 &&
              olen == 32;
    mbedtls_ecp_point_free(&pub);
    return ok;
}

// Fails on a malformed or low-order phone key (mbedtls rejects an all-zero
// shared secret). The ephemeral private key is wiped either way.
static bool pairing_finish(const uint8_t watch_pub[32], const uint8_t phone_pub[32], uint8_t key_out[32])
{
    mbedtls_ecp_point peer;
    mbedtls_mpi z;
    mbedtls_ecp_point_init(&peer);
    mbedtls_mpi_init(&z);
    uint8_t shared[32];
    bool ok = mbedtls_ecp_point_read_binary(&pair_grp, &peer, phone_pub, 32) == 0 &&
              mbedtls_ecdh_compute_shared(&pair_grp, &z, &peer, &pair_priv, hw_rng, NULL) == 0 &&
              mbedtls_mpi_write_binary_le(&z, shared, 32) == 0;
    if (ok) {
        static const char label[] = "nabeeh-pair-v1";
        const size_t label_len = sizeof(label) - 1;
        uint8_t buf[sizeof(label) - 1 + 32 * 3];
        memcpy(buf, label, label_len);
        memcpy(buf + label_len, shared, 32);
        memcpy(buf + label_len + 32, watch_pub, 32);
        memcpy(buf + label_len + 64, phone_pub, 32);
        ok = mbedtls_sha256(buf, sizeof(buf), key_out, 0) == 0;
        memset(buf, 0, sizeof(buf));
    }
    memset(shared, 0, sizeof(shared));
    mbedtls_ecp_point_free(&peer);
    mbedtls_mpi_free(&z);
    mbedtls_mpi_free(&pair_priv);
    mbedtls_mpi_init(&pair_priv);
    return ok;
}

static void compute_hmac_sha256(const uint8_t *key, size_t key_len,
                                 const uint8_t *msg, size_t msg_len,
                                 uint8_t *out32)
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, info, 1 /* HMAC */);
    mbedtls_md_hmac_starts(&ctx, key, key_len);
    mbedtls_md_hmac_update(&ctx, msg, msg_len);
    mbedtls_md_hmac_finish(&ctx, out32);
    mbedtls_md_free(&ctx);
}

static void bytes_to_hex(const uint8_t *data, size_t len, char *out_hex /* len*2+1 bytes */)
{
    static const char *digits = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out_hex[i * 2]     = digits[data[i] >> 4];
        out_hex[i * 2 + 1] = digits[data[i] & 0x0F];
    }
    out_hex[len * 2] = '\0';
}

static bool hex_to_bytes(const char *hex, size_t hex_len, uint8_t *out, size_t out_len)
{
    if (hex_len != out_len * 2) return false;
    for (size_t i = 0; i < out_len; i++) {
        char c1 = hex[i * 2], c2 = hex[i * 2 + 1];
        int hi = isdigit((unsigned char)c1) ? c1 - '0' : (tolower(c1) - 'a' + 10);
        int lo = isdigit((unsigned char)c2) ? c2 - '0' : (tolower(c2) - 'a' + 10);
        if (hi < 0 || hi > 15 || lo < 0 || lo > 15) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static void save_paired_key(const uint8_t key[DEVICE_KEY_LEN])
{
    memcpy(device_key, key, DEVICE_KEY_LEN);
    device_paired = true;
    wifi_prefs.begin("nabeeh", false);
    wifi_prefs.putBytes("dev_key", device_key, DEVICE_KEY_LEN);
    wifi_prefs.putBool("paired", true);
    wifi_prefs.end();
}

// يلغي المفتاح الحالي (يستبدله بقيمة عشوائية ما يعرفها أحد، فالجوال القديم
// ينرفض فورًا) ويفتح نافذة إقران جديدة — أول إقلاع وزر "إلغاء ارتباط الهاتف".
static void forget_pairing()
{
    esp_fill_random(device_key, DEVICE_KEY_LEN);
    device_paired = false;
    wifi_prefs.begin("nabeeh", false);
    wifi_prefs.putBytes("dev_key", device_key, DEVICE_KEY_LEN);
    wifi_prefs.putBool("paired", false);
    wifi_prefs.remove("key_delivered"); // leftover from the plaintext-PAIR build
    wifi_prefs.end();
    Serial.println("Security: pairing cleared — the next phone to connect will pair via X25519.");
}

// يُستدعى مرة وحدة عند الإقلاع (network_task).
static void ensure_device_key()
{
    mbedtls_ecp_group_init(&pair_grp);
    mbedtls_mpi_init(&pair_priv);

    wifi_prefs.begin("nabeeh", true);
    size_t got = wifi_prefs.getBytesLength("dev_key");
    bool paired = wifi_prefs.getBool("paired", false);
    if (got == DEVICE_KEY_LEN) {
        wifi_prefs.getBytes("dev_key", device_key, DEVICE_KEY_LEN);
        device_paired = paired;
        wifi_prefs.end();
        Serial.printf("Security: %s.\n", device_paired ? "paired" : "not paired yet — waiting for a phone to pair");
        return;
    }
    wifi_prefs.end();
    forget_pairing();
}

// يرجّع يوم الأسبوع ١=الاثنين .. ٧=الأحد (خوارزمية Sakamoto). نحسبه من
// التاريخ بأنفسنا بدل ما نقرأ حقل يوم الأسبوع من الـRTC، لأن ذاك الحقل سجل
// منفصل بالشريحة ولا يتحدّث تلقائيًا لما نضبط الوقت من NTP.
static int weekday_iso(int y, int m, int d)
{
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    int w = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7; // 0=الأحد
    return w == 0 ? 7 : w;
}

// يفكّ "H:M:mask:pattern:intensity:once;H:M:..." — نص فاضي يعني مسح الجدول.
// أي مقطع مشوّه يوقف القراءة عنده بدل ما تدخل بيانات نصف صحيحة للجدول.
// يرجّع عدد التذكيرات بعد التحديث، أو ‎-1‎ لو رفض الرسالة وأبقى الجدول القديم.
static int parse_reminder_payload(const char *payload)
{
    if (!payload) return -1;

    // نفكّ لمصفوفة مؤقتة أولًا، وما نلمس الجدول الشغّال إلا لما نتأكد إن
    // الرسالة سليمة. النسخة السابقة كانت تصفّر الجدول قبل ما تبدأ الفك، فأي
    // رسالة وصلت ناقصة أو مشوّهة كانت تمسح كل التذكيرات بصمت.
    WatchReminder parsed[MAX_WATCH_REMINDERS];
    int count = 0;

    const char *p = payload;
    while (*p && count < MAX_WATCH_REMINDERS) {
        int h = -1, m = -1, mask = -1, once = 0, consumed = 0;
        char pat = 0, inten = 0;
        if (sscanf(p, "%d:%d:%d:%c:%c:%d%n", &h, &m, &mask, &pat, &inten, &once, &consumed) != 6) {
            Serial.printf("Reminders: مقطع غير صالح عند \"%.24s\" — توقفنا هنا\n", p);
            break;
        }
        if (h < 0 || h > 23 || m < 0 || m > 59 || mask <= 0 || mask > 0x7F ||
            (pat != '1' && pat != '2' && pat != '3') ||
            (inten != '1' && inten != '2' && inten != '3')) {
            Serial.printf("Reminders: قيم خارج المدى عند \"%.24s\" — توقفنا هنا\n", p);
            break;
        }

        WatchReminder &r = parsed[count++];
        r.hour = (uint8_t)h;
        r.minute = (uint8_t)m;
        r.days_mask = (uint8_t)mask;
        r.pattern = pat;
        r.intensity = inten;
        r.once = (once != 0);
        r.spent = false;
        r.label[0] = '\0';

        p += consumed;

        // الحقل السابع (اسم التذكير) اختياري وآخر حقل بالمقطع، فيقدر يحتوي
        // ':' بدون لبس — ';' وحده هو الفاصل بين المقاطع. اختياريّته مقصودة:
        // جدول محفوظ بصيغة أقدم (بدون اسم) يظل يُقرأ بدل ما يُرفض كله.
        if (*p == ':') {
            p++;
            size_t li = 0;
            while (*p && *p != ';') {
                if (li < sizeof(r.label) - 1) r.label[li++] = *p;
                p++;
            }
            r.label[li] = '\0';
            trim_incomplete_utf8(r.label);
        }

        Serial.printf("Reminders:   [%d] %02d:%02d mask=0x%02X نمط=%c شدة=%c مرة_وحدة=%d اسم=\"%s\"\n",
                      count - 1, r.hour, r.minute, r.days_mask,
                      r.pattern, r.intensity, r.once ? 1 : 0, r.label);

        if (*p != ';') break;
        p++;
    }

    // نص فاضي = طلب مسح صريح من التطبيق (آخر تذكير انحذف) — نحترمه.
    // لكن نص فيه بيانات وما طلع منه ولا تذكير صالح = رسالة مشوّهة، فنتمسّك
    // بالجدول القديم: مسحه يسكّت كل التذكيرات بدون ما يلاحظ أحد.
    if (count == 0 && payload[0] != '\0') {
        Serial.printf("Reminders: رسالة مشوّهة — نبقي الجدول الحالي (%d تذكير)\n",
                      watch_reminder_count);
        return -1;
    }

    memcpy(watch_reminders, parsed, sizeof(WatchReminder) * count);
    watch_reminder_count = count;
    return count;
}

static lv_obj_t *splash_screen;
static lv_obj_t *onboarding_screen;
static lv_obj_t *home_screen;
static lv_obj_t *settings_screen = NULL;
static lv_obj_t *wifi_settings_screen = NULL;
static lv_obj_t *wifi_current_network_lbl = NULL;
static lv_obj_t *wifi_saved_list = NULL;         // scrollable column of saved-network rows, rebuilt by refresh_wifi_saved_list()
static lv_obj_t *wifi_reset_confirm_overlay = NULL;
// Set by the Settings screen's "إلغاء ارتباط الهاتف" confirm (UI task);
// network_task calls forget_pairing() and drops the current TCP client, since
// it owns that socket.
static volatile bool security_reset_requested = false;
static lv_obj_t *alert_screen = NULL;
static lv_obj_t *clock_label;
static lv_obj_t *date_label;
static lv_obj_t *batt_pct_label;
static lv_obj_t *batt_icon_label;
static lv_obj_t *status_label;
static lv_obj_t *status_dot;
static lv_obj_t *wifi_status_box = NULL;   // colored callout card wrapping wifi_status_label — makes the state change hard to miss, not just different text
static lv_obj_t *wifi_status_label = NULL; // Settings screen feedback for the "change Wi-Fi" button; NULL until build_settings_screen() runs
static bool sign_language_mode = true; // matches the pre-selected option in onboarding
// Was: a manual "pairing preference" toggled only by the Settings screen's
// disconnect button, defaulting to true (claiming "connected" even right
// after boot, before any phone had ever talked to the watch) and never
// otherwise updated — so the on-screen dot was disconnected from reality.
// Now: network_task (core 0) sets this from the real TCP client state
// (see the client-connect/disconnect handling in its loop) and the UI task
// only polls it (see update_connection_status_poll_cb, a timer in setup())
// — same cross-task ownership pattern as portal_ready_for_ui.
static volatile bool phone_connected = false;
// Set by the Wi-Fi settings screen's "مسح كل الشبكات المحفوظة" confirm
// (UI task, clears saved_networks[] itself immediately since NVS access
// isn't task-restricted in this file); network_task consumes this flag to
// actually drop the radio connection and stop retrying the just-cleared
// credentials.
static volatile bool wifi_reset_requested = false;

// Wi-Fi settings screen: "only show saved networks that are actually in
// range right now" — network_task (the task that owns all WiFi.* calls in
// this file) runs the scan and drops the raw SSID list here; the UI task
// only ever reads nearby_ssids[]/nearby_ssid_count after wifi_scan_ready
// flips true, so there's no concurrent read/write on them despite crossing
// tasks. Scanning while connected briefly interrupts the radio (the ESP32
// hops channels to scan), so this only ever runs when the user explicitly
// opens the Wi-Fi settings screen — never in the background — to keep that
// cost rare and expected rather than a surprise mid-stream.
static volatile bool wifi_scan_for_settings_requested = false;
static volatile bool wifi_scan_for_settings_ready = false;
#define MAX_SCAN_RESULTS 32
static String nearby_ssids[MAX_SCAN_RESULTS];
static int nearby_ssid_count = 0;

#define COLOR_CONNECTED    lv_color_hex(0x35D07F)
#define COLOR_DISCONNECTED lv_color_hex(0xE05A4E)

static void build_settings_screen();
static void dismiss_alert_cb(lv_event_t *e); // defined next to alert_return_timer, further down
static void change_wifi_cb(lv_event_t *e); // defined near start_ble_provisioning(), further down
static void cancel_wifi_setup_cb(lv_event_t *e); // defined alongside change_wifi_cb, further down
static void build_wifi_settings_screen(); // defined next to go_to_wifi_settings_cb(), further down
static void refresh_wifi_saved_list(); // defined next to build_wifi_settings_screen(), further down
static void go_to_wifi_settings_cb(lv_event_t *e); // defined next to build_wifi_settings_screen(), further down
static void connect_to_saved_network_cb(lv_event_t *e); // needs pending_wifi_ssid/wifi_credentials_pending, defined further down alongside change_wifi_cb
static void show_wifi_reset_confirm_cb(lv_event_t *e); // defined next to build_wifi_settings_screen(), further down

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

// phone_connected now changes asynchronously from network_task (core 0),
// not just from a UI button tap — so unlike before, the dot/label need to
// be actively polled to stay in sync, not just repainted on specific UI
// events. status_label/status_dot are both built unconditionally in
// build_home_screen() at boot, so no null-guard needed here (unlike
// wifi_status_box, which only exists once Settings has been opened once).
static void update_connection_status_poll_cb(lv_timer_t *t)
{
    update_connection_status();
}

// wifi_current_network_lbl only exists once the Wi-Fi settings screen has
// been opened once (built lazily by go_to_wifi_settings_cb) — same
// null-guard reasoning as wifi_status_box above.
static void update_wifi_current_network_display_cb(lv_timer_t *t)
{
    if (!wifi_current_network_lbl) return;
    if (WiFi.status() == WL_CONNECTED) {
        lv_label_set_text(wifi_current_network_lbl, WiFi.SSID().c_str());
    } else {
        lv_label_set_text(wifi_current_network_lbl, "غير متصلة");
    }
}

// يرصد لحظة رجوع نتيجة مسح الشبكات القريبة (نفس نمط was_ready اللي تستخدمه
// update_wifi_status_display_cb فوق) ويعيد بناء القائمة عندها — قبل هذا،
// refresh_wifi_saved_list() نفسها تكون عارضة "جارٍ البحث...".
static void update_wifi_scan_result_poll_cb(lv_timer_t *t)
{
    static bool was_ready = false;
    if (wifi_scan_for_settings_ready && !was_ready) {
        refresh_wifi_saved_list();
    }
    was_ready = wifi_scan_for_settings_ready;
}

static lv_obj_t *confirm_modal_overlay = NULL;

static void close_confirm_modal_cb(lv_event_t *e)
{
    if (confirm_modal_overlay) {
        lv_obj_delete(confirm_modal_overlay);
        confirm_modal_overlay = NULL;
    }
}

// Replaces the old "فصل الساعة عن الجوال", which only closed the socket: with
// the app holding a persistent connection it reconnected within ~2s, so the
// button did nothing visible — and a disconnect that *stayed* disconnected
// would silently cut off sound alerts for a deaf user. Unpairing is the thing
// the button actually meant: this phone can't reconnect until it pairs again.
static void confirm_unpair_cb(lv_event_t *e)
{
    security_reset_requested = true;
    close_confirm_modal_cb(e);
}

static void show_unpair_confirm_cb(lv_event_t *e)
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
    lv_label_set_text(question, "إلغاء ارتباط الهاتف؟ لازم تربطين التطبيق من جديد بعدها.");
    lv_obj_set_style_text_font(question, &tajawal_regular_16, 0);
    lv_obj_set_style_text_color(question, COLOR_TEXT, 0);
    lv_obj_set_width(question, lv_pct(100));
    lv_obj_set_style_text_align(question, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *yes_btn = lv_button_create(card);
    lv_obj_set_size(yes_btn, lv_pct(100), 36);
    lv_obj_set_style_bg_color(yes_btn, COLOR_DISCONNECTED, 0);
    lv_obj_set_style_bg_opa(yes_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(yes_btn, 10, 0);
    lv_obj_add_event_cb(yes_btn, confirm_unpair_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *yes_lbl = lv_label_create(yes_btn);
    lv_label_set_text(yes_lbl, "نعم، ألغِ الارتباط");
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

// آخر قراءة للـRTC، تتحدّث مرة كل ثانية من مؤقّت الساعة تحت.
//
// أي كود ثاني يحتاج الوقت لازم يقرأ من هنا، ما يفتح قراءة I2C جديدة:
// الـRTC على نفس ناقل الـI2C اللي عليه الـPMU ومحرك الاهتزاز DRV2605 (انظر
// التعليق الطويل عند result_queue). قراءة الـRTC من داخل loop() مباشرة تعني
// مئات المعاملات بالثانية على الناقل، وأول ما يشتغل تنبيه ويهتز المحرك
// تتزاحم معه وتعلّق الناقل — وبعدها كل قراءات الـRTC ترجع قيمًا فاسدة،
// فالساعة تصير ما تعرف كم الوقت وما يشتغل أي تذكير بعدها. مرة كل ثانية
// (وهو المعدل اللي كان شغّالًا أصلًا وما سبّب مشاكل) آمن ووفير.
struct RtcSnapshot {
    int year, month, day, hour, minute;
    bool valid;
};
static RtcSnapshot rtc_snapshot = {0, 0, 0, 0, 0, false};

static void update_clock_display_cb(lv_timer_t *t)
{
    RTC_DateTime now = instance.rtc.getDateTime();
    rtc_snapshot = {now.getYear(),  now.getMonth(), now.getDay(),
                    now.getHour(),  now.getMinute(), true};

    // This runs once a second (see the lv_timer_create call in setup) so the
    // displayed minute flips within a second of the real one. It used to run
    // every 30s, which meant the face could legitimately sit up to half a
    // minute behind the true time — the RTC was right, the screen just hadn't
    // caught up yet, which reads to a user as "the watch is ~30 seconds late".
    // Everything below is guarded so a tick where nothing changed costs one
    // I2C read and no redraw at all.
    static int last_minute_shown = -1;
    static int last_day_shown = -1;

    if (now.getMinute() != last_minute_shown) {
        last_minute_shown = now.getMinute();
        char time_buf[8];
        snprintf(time_buf, sizeof(time_buf), "%02d:%02d", now.getHour(), now.getMinute());
        lv_label_set_text(clock_label, time_buf);
    }

    if (now.getDay() != last_day_shown) {
        last_day_shown = now.getDay();
        char day_buf[8];
        arabic_digits(now.getDay(), day_buf);
        char date_buf[40];
        snprintf(date_buf, sizeof(date_buf), "%s %s", day_buf,
                 arabic_month_names[now.getMonth() >= 1 && now.getMonth() <= 12 ? now.getMonth() - 1 : 0]);
        lv_label_set_text(date_label, date_buf);
    }

    // The battery is worth polling far less often than the clock: the reading
    // moves slowly and it's a second I2C transaction on top of the RTC read
    // above. Keep it on roughly the original 30s cadence.
    static int batt_poll_countdown = 0;
    if (batt_poll_countdown > 0) {
        batt_poll_countdown--;
        return;
    }
    batt_poll_countdown = 30;

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

    // زر إغلاق (X) بدل سهم الرجوع: التنبيه الآن يقعد على الشاشة دقيقة كاملة
    // (انظر REMINDER_AUTO_RETURN_MS)، فلازم يكون واضح إن فيه طريقة تصرفه
    // بنفسك. دائرة خفيفة خلف العلامة عشان يبان إنه زر يُضغط، مو مجرد رمز.
    lv_obj_t *close_btn = lv_button_create(alert_screen);
    lv_obj_set_size(close_btn, 40, 40);
    lv_obj_set_style_radius(close_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_20, 0);
    lv_obj_set_style_shadow_width(close_btn, 0, 0);
    lv_obj_set_style_pad_all(close_btn, 0, 0);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -6, 6);
    lv_obj_add_event_cb(close_btn, dismiss_alert_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_icon = lv_label_create(close_btn);
    lv_label_set_text(close_icon, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(close_icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(close_icon, COLOR_TEXT, 0);
    lv_obj_center(close_icon);

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

    // نفس زر الإغلاق اللي بشاشة التنبيه العادية — الشاشتين تظهران لنفس
    // السبب، فلازم تنصرفان بنفس الطريقة.
    lv_obj_t *close_btn = lv_button_create(sign_screen);
    lv_obj_set_size(close_btn, 40, 40);
    lv_obj_set_style_radius(close_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_20, 0);
    lv_obj_set_style_shadow_width(close_btn, 0, 0);
    lv_obj_set_style_pad_all(close_btn, 0, 0);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -6, 6);
    lv_obj_add_event_cb(close_btn, dismiss_alert_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_icon = lv_label_create(close_btn);
    lv_label_set_text(close_icon, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(close_icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(close_icon, COLOR_TEXT, 0);
    lv_obj_center(close_icon);

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

// label_override: اسم التذكير لو فيه واحد. أي شي ثاني (تنبيهات تصنيف الصوت)
// يمرّر nullptr فيُعرض اسم الفئة العام زي قبل.
static void show_alert(int category_index, const char *label_override = nullptr)
{
    const AlertCategory &cat = alert_categories[category_index];
    // Some categories (currently just "تذكير") have no sign-language video
    // yet — cat.sign_gif is nullptr for those. Falling through to
    // show_sign_result() with a null GIF source would either show nothing
    // useful or misbehave, so those always use the plain icon+text alert
    // below regardless of sign_language_mode.
    if (sign_language_mode && cat.sign_gif != nullptr) {
        show_sign_result(category_index);
        return;
    }

    if (!alert_screen) {
        build_alert_screen();
    }
    lv_image_set_src(alert_icon_img, cat.icon);
    lv_label_set_text(alert_category_lbl,
                      (label_override && label_override[0]) ? label_override : cat.label);
    lv_screen_load_anim(alert_screen, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
    lv_anim_delete(alert_pulse_ring, alert_pulse_anim_cb);
    lv_anim_start(&alert_pulse_anim);
}

// Auto-return to Home a few seconds after a result is shown, so a detected
// sound doesn't sit on screen forever waiting for the user to back out.
static lv_timer_t *alert_return_timer = NULL;
#define ALERT_AUTO_RETURN_MS 6000
// التذكير يقعد أطول بكثير من تنبيه صوت: تنبيه الصوت لحظي (جرس يرن الحين)،
// أما التذكير فالمستخدم ممكن ما يكون شايف الساعة لحظة اهتزازها، ولو راحت
// الشاشة بعد ٦ ثواني ما بيعرف وش كان التذكير أصلًا. زر الـX يصرفه قبلها.
#define REMINDER_AUTO_RETURN_MS 60000

// صرف التنبيه يدويًا من زر الـX. نلغي مؤقّت الرجوع التلقائي معه، وإلا ضل
// شغّالًا وسحب المستخدم للرئيسية فجأة وهو بشاشة ثانية بعد ما صرف التنبيه.
static void dismiss_alert_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (alert_return_timer) {
        lv_timer_delete(alert_return_timer);
        alert_return_timer = NULL;
    }
    lv_screen_load_anim(home_screen, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
}

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
    char label[64]; // نص يُعرض بدل اسم الفئة (اسم التذكير)؛ فاضي = اسم الفئة
};
static QueueHandle_t result_queue;

static void show_result_category(const ResultMessage &msg)
{
    const int category_index = msg.category_index;
    show_alert(category_index, msg.label);
    if (alert_return_timer) {
        lv_timer_delete(alert_return_timer);
    }
    uint32_t hold_ms = (category_index == reminder_category_index())
                           ? REMINDER_AUTO_RETURN_MS
                           : ALERT_AUTO_RETURN_MS;
    alert_return_timer = lv_timer_create(alert_return_timeout_cb, hold_ms, NULL);
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
    // يكرر نفس التأثير مرتين بفاصل ٤٠٠ms — جُرّب بمقارنة مباشرة مع ٣ تكرارات
    // وفُضّل عليها (الإحساس التراكمي بمرتين كان أوضح فعليًا من ٣). محاولة
    // سابقة بفاصل قصير (150ms) سبّبت طوفان أخطاء I2C ("probe failed")
    // متلاحقة لأن الموجة السابقة ما كانت تخلص تشغيلها فعليًا قبل الطلب
    // الجديد؛ ٤٠٠ms مؤكد آمن (صفر أخطاء بعدة اختبارات فعلية).
    instance.vibrator();
    delay(400);
    instance.vibrator();
}

// اسم التذكير المطابق للدقيقة الحالية. لازم لأن تنبيه '#T' الجاي من الجوال
// ثابت الطول (٤ بايت) وما يقدر يحمل اسمًا — فناخذ الاسم من الجدول المخزّن
// عندنا. ±دقيقة عشان فرق بسيط بين ساعة الجوال وساعة الساعة ما يضيّع الاسم.
static const char *lookup_reminder_label_for_now()
{
    const RtcSnapshot now = rtc_snapshot;
    if (!now.valid || watch_reminder_count == 0) return nullptr;

    const int now_min = now.hour * 60 + now.minute;
    for (int i = 0; i < watch_reminder_count; i++) {
        const WatchReminder &r = watch_reminders[i];
        if (r.label[0] == '\0') continue;
        int diff = now_min - (r.hour * 60 + r.minute);
        if (diff < 0) diff = -diff;
        if (diff > 1 && diff < 1439) continue; // 1439 = لفّة منتصف الليل
        return r.label;
    }
    return nullptr;
}

// Called when a full '#'+category+pattern+intensity sequence arrives (real,
// from the phone over Wi-Fi, on the network task; or simulated, typed into
// Serial on the UI task for testing) — safe to call from either, since it
// only ever queues a message rather than acting on it directly.
static void handle_result_code(char code, char pattern, char intensity, const char *source,
                               const char *label = nullptr)
{
    // التذكير له مصدرين مقصودين (ساعة الساعة نفسها، والجوال لو كان صاحيًا) —
    // نكتم الثاني عشان المستخدم ما يحس باهتزازين لنفس التذكير. يخص 'T' فقط:
    // تنبيهات تصنيف الصوت ممكن تتكرر بشكل مشروع (طرق متتالي مثلًا).
    if (code == 'T') {
        unsigned long now_ms = millis();
        if (last_reminder_dispatch_ms != 0 &&
            now_ms - last_reminder_dispatch_ms < REMINDER_DUPLICATE_WINDOW_MS) {
            Serial.printf("Result (%s): تذكير مكرر بعد %lu ثانية — تجاهل\n",
                          source, (now_ms - last_reminder_dispatch_ms) / 1000);
            return;
        }
        last_reminder_dispatch_ms = now_ms;
    }

    for (size_t i = 0; i < RESULT_CODE_COUNT; i++) {
        if (result_codes[i].code != code) continue;

        ResultMessage msg = {result_codes[i].category_index, pattern, intensity, {0}};

        // تنبيه تذكير جاي من الجوال ما يحمل اسمًا (بروتوكول '#' ثابت الطول)
        // — ندوّر على التذكير المطابق للوقت الحالي بالجدول وناخذ اسمه منه.
        const char *shown = label;
        if (code == 'T' && (shown == nullptr || shown[0] == '\0')) {
            shown = lookup_reminder_label_for_now();
        }
        if (shown && shown[0]) {
            strncpy(msg.label, shown, sizeof(msg.label) - 1);
            msg.label[sizeof(msg.label) - 1] = '\0';
            trim_incomplete_utf8(msg.label);
        }

        Serial.printf("Result (%s): '%c' -> %s%s%s (pattern=%c, intensity=%c)\n",
                      source, code, alert_categories[msg.category_index].label,
                      msg.label[0] ? " — " : "", msg.label, pattern, intensity);
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
    lv_obj_t *unpair_btn = lv_button_create(content);
    lv_obj_set_size(unpair_btn, lv_pct(100), 36);
    lv_obj_set_style_bg_opa(unpair_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(unpair_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(unpair_btn, LV_OPA_30, 0);
    lv_obj_set_style_border_width(unpair_btn, 1, 0);
    lv_obj_set_style_radius(unpair_btn, 10, 0);
    lv_obj_set_style_margin_top(unpair_btn, 6, 0);
    lv_obj_add_event_cb(unpair_btn, show_unpair_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *unpair_lbl = lv_label_create(unpair_btn);
    lv_label_set_text(unpair_lbl, "إلغاء ارتباط الهاتف");
    lv_obj_set_style_text_font(unpair_lbl, &tajawal_regular_16, 0);
    lv_obj_set_style_text_color(unpair_lbl, COLOR_TEXT, 0);
    lv_obj_center(unpair_lbl);

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
    lv_obj_add_event_cb(change_wifi_btn, go_to_wifi_settings_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *change_wifi_lbl = lv_label_create(change_wifi_btn);
    lv_label_set_text(change_wifi_lbl, "شبكة الواي فاي");
    lv_obj_set_style_text_font(change_wifi_lbl, &tajawal_regular_16, 0);
    lv_obj_set_style_text_color(change_wifi_lbl, COLOR_TEXT, 0);
    lv_obj_center(change_wifi_lbl);

}

// يعيد بناء صفوف الشبكات المحفوظة داخل wifi_saved_list من saved_networks[]
// الحالية. يُستدعى كل ما تُفتح الشاشة (عشان يعكس شبكة جديدة انضافت من زر
// "تغيير الشبكة") وبعد أي تغيير على القائمة (اتصال بشبكة محفوظة، أو مسح الكل).
// يعرض صف واحد فاضٍ برسالة عامة (مستخدم لحالتي "لا توجد شبكات محفوظة" و
// "لا توجد شبكات بالمدى") — تكرار الأربع أسطر نفسها ثلاث مرات ما كان يستاهل.
static void show_wifi_list_message(const char *text)
{
    lv_obj_t *lbl = lv_label_create(wifi_saved_list);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &tajawal_regular_16, 0);
    lv_obj_set_style_text_color(lbl, COLOR_MUTED, 0);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
}

// يعيد بناء صفوف الشبكات المحفوظة داخل wifi_saved_list من saved_networks[]،
// لكن يعرض بس اللي منها ظاهرة بآخر مسح (nearby_ssids[] — انظر تعليق
// wifi_scan_for_settings_requested) عشان المستخدم ما يحاول يتصل بشبكة بعيدة
// عن غير قصد. الشبكة المتصلة بها الآن تظهر دايمًا حتى لو غابت عن المسح
// الأخير (فرق توقيت بسيط بين المسح والاتصال الفعلي، نادر لكن ممكن).
static void refresh_wifi_saved_list()
{
    if (!wifi_saved_list) return;
    lv_obj_clean(wifi_saved_list); // يشيل كل الصفوف القديمة قبل ما نعيد بناءها

    if (saved_network_count == 0) {
        show_wifi_list_message("لا توجد شبكات محفوظة");
        return;
    }
    if (!wifi_scan_for_settings_ready) {
        show_wifi_list_message("جارٍ البحث عن الشبكات القريبة...");
        return;
    }

    bool wifi_up = (WiFi.status() == WL_CONNECTED);
    int shown = 0;
    for (int i = 0; i < saved_network_count; i++) {
        bool is_current = wifi_up && WiFi.SSID() == saved_networks[i].ssid;
        bool nearby = is_current;
        for (int s = 0; !nearby && s < nearby_ssid_count; s++) {
            if (nearby_ssids[s] == saved_networks[i].ssid) nearby = true;
        }
        if (!nearby) continue;
        shown++;

        lv_obj_t *row = lv_button_create(wifi_saved_list);
        lv_obj_set_size(row, lv_pct(100), 40);
        lv_obj_set_style_bg_opa(row, LV_OPA_10, 0);
        lv_obj_set_style_bg_color(row, lv_color_white(), 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_radius(row, 10, 0);
        // i يُمرَّر كـuser_data عشان connect_to_saved_network_cb يعرف أي شبكة
        // بالضبط انضغطت — نفس فكرة تمرير فهرس لعنصر بقائمة ديناميكية.
        lv_obj_add_event_cb(row, connect_to_saved_network_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *row_lbl = lv_label_create(row);
        char buf[96];
        snprintf(buf, sizeof(buf), "%s%s", saved_networks[i].ssid, is_current ? "  (متصلة الآن)" : "");
        lv_label_set_text(row_lbl, buf);
        lv_obj_set_style_text_font(row_lbl, &tajawal_regular_16, 0);
        lv_obj_set_style_text_color(row_lbl, is_current ? lv_color_hex(0x35D07F) : COLOR_TEXT, 0);
        lv_obj_center(row_lbl);
    }
    if (shown == 0) {
        show_wifi_list_message("لا توجد شبكات محفوظة بالمدى حاليًا");
    }
}

static void go_to_wifi_settings_cb(lv_event_t *e)
{
    if (!wifi_settings_screen) {
        build_wifi_settings_screen();
    }
    // كل ما نفتح الشاشة نطلب مسح جديد — نتيجة مسح قديمة ممكن تكون قديمة
    // (شبكة راحت من المدى أو جت وحدة جديدة) — و"جارٍ البحث" تبان فورًا لين
    // يرد network_task (انظر update_wifi_scan_result_poll_cb بـsetup()).
    wifi_scan_for_settings_ready = false;
    wifi_scan_for_settings_requested = true;
    refresh_wifi_saved_list();
    lv_screen_load_anim(wifi_settings_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

static void go_to_settings_from_wifi_cb(lv_event_t *e)
{
    lv_screen_load_anim(settings_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}

static void close_wifi_reset_confirm_cb(lv_event_t *e)
{
    if (wifi_reset_confirm_overlay) {
        lv_obj_delete(wifi_reset_confirm_overlay);
        wifi_reset_confirm_overlay = NULL;
    }
}

// المسح نفسه (clear_saved_networks + تحديث القائمة) يصير هنا فورًا على مهمة
// الواجهة — نفس ما ذكرناه بتعليق wifi_reset_requested: NVS مو مقيدة بمهمة
// معينة بهذا الملف. wifi_reset_requested يبقى فقط لطلب قطع الراديو الفعلي
// من مهمة الشبكة (تلك اللي تملك WiFi.disconnect()/الاتصال الحالي).
static void confirm_wifi_reset_cb(lv_event_t *e)
{
    clear_saved_networks();
    refresh_wifi_saved_list();
    wifi_reset_requested = true;
    close_wifi_reset_confirm_cb(e);
}

static void show_wifi_reset_confirm_cb(lv_event_t *e)
{
    lv_obj_t *parent = lv_screen_active();

    wifi_reset_confirm_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(wifi_reset_confirm_overlay);
    lv_obj_set_size(wifi_reset_confirm_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(wifi_reset_confirm_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(wifi_reset_confirm_overlay, LV_OPA_60, 0);
    lv_obj_center(wifi_reset_confirm_overlay);

    lv_obj_t *card = lv_obj_create(wifi_reset_confirm_overlay);
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
    lv_label_set_text(question, "هل فعلاً تبي تمسح كل الشبكات المحفوظة؟");
    lv_obj_set_style_text_font(question, &tajawal_regular_16, 0);
    lv_obj_set_style_text_color(question, COLOR_TEXT, 0);
    lv_obj_set_width(question, lv_pct(100));
    lv_obj_set_style_text_align(question, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *yes_btn = lv_button_create(card);
    lv_obj_set_size(yes_btn, lv_pct(100), 36);
    lv_obj_set_style_bg_color(yes_btn, COLOR_DISCONNECTED, 0);
    lv_obj_set_style_bg_opa(yes_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(yes_btn, 10, 0);
    lv_obj_add_event_cb(yes_btn, confirm_wifi_reset_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *yes_lbl = lv_label_create(yes_btn);
    lv_label_set_text(yes_lbl, "نعم، امسح الكل");
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
    lv_obj_add_event_cb(no_btn, close_wifi_reset_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *no_lbl = lv_label_create(no_btn);
    lv_label_set_text(no_lbl, "لا");
    lv_obj_set_style_text_font(no_lbl, &tajawal_regular_16, 0);
    lv_obj_set_style_text_color(no_lbl, COLOR_TEXT, 0);
    lv_obj_center(no_lbl);
}

static void build_wifi_settings_screen()
{
    wifi_settings_screen = make_screen();

    lv_obj_t *header = lv_obj_create(wifi_settings_screen);
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
    lv_obj_add_event_cb(back_btn, go_to_settings_from_wifi_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_icon = lv_label_create(back_btn);
    lv_label_set_text(back_icon, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(back_icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(back_icon, COLOR_TEXT, 0);
    lv_obj_center(back_icon);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "الإنترنت ");
    lv_obj_set_style_text_font(title, &tajawal_bold_24, 0);
    lv_obj_set_style_text_color(title, COLOR_TEXT, 0);
    lv_obj_align_to(title, back_btn, LV_ALIGN_OUT_LEFT_MID, -6, 0);

    lv_obj_t *content = lv_obj_create(wifi_settings_screen);
    lv_obj_remove_style_all(content);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(content, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_align_to(content, header, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 10, 0);

    // الشبكة الحالية
    lv_obj_t *current_section = lv_label_create(content);
    lv_label_set_text(current_section, "الشبكة الحالية");
    lv_obj_set_style_text_font(current_section, &tajawal_regular_16, 0);
    lv_obj_set_style_text_color(current_section, COLOR_MUTED, 0);
    lv_obj_set_width(current_section, lv_pct(100));
    lv_obj_set_style_text_align(current_section, LV_TEXT_ALIGN_RIGHT, 0);

    wifi_current_network_lbl = lv_label_create(content);
    lv_label_set_text(wifi_current_network_lbl, "..."); // update_wifi_current_network_poll_cb (setup()) يملأها فورًا
    lv_obj_set_style_text_font(wifi_current_network_lbl, &tajawal_bold_24, 0);
    lv_obj_set_style_text_color(wifi_current_network_lbl, COLOR_TEXT, 0);
    lv_obj_set_width(wifi_current_network_lbl, lv_pct(100));
    lv_obj_set_style_text_align(wifi_current_network_lbl, LV_TEXT_ALIGN_RIGHT, 0);

    // تغيير الشبكة (يضيف شبكة جديدة عبر نفس تدفق BLE/نقطة الوصول الحالي)
    lv_obj_t *change_btn = lv_button_create(content);
    lv_obj_set_size(change_btn, lv_pct(100), 36);
    lv_obj_set_style_bg_opa(change_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(change_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(change_btn, LV_OPA_30, 0);
    lv_obj_set_style_border_width(change_btn, 1, 0);
    lv_obj_set_style_radius(change_btn, 10, 0);
    lv_obj_set_style_margin_top(change_btn, 6, 0);
    lv_obj_add_event_cb(change_btn, change_wifi_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *change_lbl = lv_label_create(change_btn);
    lv_label_set_text(change_lbl, "تغيير الشبكة");
    lv_obj_set_style_text_font(change_lbl, &tajawal_regular_16, 0);
    lv_obj_set_style_text_color(change_lbl, COLOR_TEXT, 0);
    lv_obj_center(change_lbl);

    // مسح كل الشبكات المحفوظة
    lv_obj_t *reset_btn = lv_button_create(content);
    lv_obj_set_size(reset_btn, lv_pct(100), 36);
    lv_obj_set_style_bg_opa(reset_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(reset_btn, COLOR_DISCONNECTED, 0);
    lv_obj_set_style_border_opa(reset_btn, LV_OPA_60, 0);
    lv_obj_set_style_border_width(reset_btn, 1, 0);
    lv_obj_set_style_radius(reset_btn, 10, 0);
    lv_obj_add_event_cb(reset_btn, show_wifi_reset_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *reset_lbl = lv_label_create(reset_btn);
    lv_label_set_text(reset_lbl, "مسح كل الشبكات المحفوظة");
    lv_obj_set_style_text_font(reset_lbl, &tajawal_regular_16, 0);
    lv_obj_set_style_text_color(reset_lbl, COLOR_DISCONNECTED, 0);
    lv_obj_center(reset_lbl);

    // الشبكات المحفوظة
    lv_obj_t *saved_section = lv_label_create(content);
    lv_label_set_text(saved_section, "الشبكات المحفوظة");
    lv_obj_set_style_text_font(saved_section, &tajawal_regular_16, 0);
    lv_obj_set_style_text_color(saved_section, COLOR_MUTED, 0);
    lv_obj_set_width(saved_section, lv_pct(100));
    lv_obj_set_style_text_align(saved_section, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_margin_top(saved_section, 6, 0);

    wifi_saved_list = lv_obj_create(content);
    lv_obj_remove_style_all(wifi_saved_list);
    lv_obj_remove_flag(wifi_saved_list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(wifi_saved_list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wifi_saved_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(wifi_saved_list, 6, 0);
    // الصفوف الفعلية تُبنى بـrefresh_wifi_saved_list()، تستدعى من go_to_wifi_settings_cb
}

// Floating toast/snackbar for the "change Wi-Fi" button, built once at boot
// (see setup()) on LVGL's global top layer rather than as part of the
// Settings screen's own scrollable content. Two reasons: (1) a card sitting
// below the button was easy to miss entirely without scrolling down to it
// on a screen this short, and (2) lv_layer_top() renders above every
// screen, so the toast stays visible even if the user backs out of
// Settings while it's still active. Same brand color (COLOR_PRIMARY)
// throughout both states — only the text changes — rather than switching
// to green for "ready", which read as an inconsistent, jarring color
// change rather than a status update.
static void build_wifi_toast()
{
    wifi_status_box = lv_obj_create(lv_layer_top());
    lv_obj_remove_flag(wifi_status_box, LV_OBJ_FLAG_CLICKABLE); // never intercepts taps meant for whatever's underneath
    lv_obj_set_width(wifi_status_box, lv_pct(85));
    lv_obj_set_height(wifi_status_box, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(wifi_status_box, COLOR_PRIMARY, 0);
    lv_obj_set_style_bg_opa(wifi_status_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(wifi_status_box, 14, 0);
    lv_obj_set_style_border_width(wifi_status_box, 0, 0);
    lv_obj_set_style_pad_all(wifi_status_box, 12, 0);
    lv_obj_set_style_pad_top(wifi_status_box, 26, 0); // extra headroom so the label clears the close button below
    lv_obj_set_style_shadow_width(wifi_status_box, 16, 0);
    lv_obj_set_style_shadow_opa(wifi_status_box, LV_OPA_40, 0);
    lv_obj_align(wifi_status_box, LV_ALIGN_BOTTOM_MID, 0, -14); // standard snackbar placement: floating near the bottom edge
    lv_obj_add_flag(wifi_status_box, LV_OBJ_FLAG_HIDDEN);

    wifi_status_label = lv_label_create(wifi_status_box);
    lv_label_set_text(wifi_status_label, "");
    lv_obj_set_style_text_font(wifi_status_label, &tajawal_regular_16, 0);
    lv_obj_set_style_text_color(wifi_status_label, COLOR_TEXT, 0);
    lv_obj_set_width(wifi_status_label, lv_pct(100));
    lv_obj_set_style_text_align(wifi_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(wifi_status_label);

    // Cancel button — lets the user back out of provisioning instead of
    // being stuck watching the toast until it either succeeds or times out.
    // Removes LV_OBJ_FLAG_CLICKABLE from wifi_status_box above only stops
    // the *box itself* from intercepting taps meant for the screen behind
    // it; a clickable child like this button still receives its own taps
    // normally regardless of the parent's flag.
    lv_obj_t *close_btn = lv_button_create(wifi_status_box);
    lv_obj_set_size(close_btn, 22, 22);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 4, -4);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(close_btn, 0, 0);
    lv_obj_set_style_pad_all(close_btn, 0, 0);
    lv_obj_add_event_cb(close_btn, cancel_wifi_setup_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_icon = lv_label_create(close_btn);
    lv_label_set_text(close_icon, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(close_icon, COLOR_TEXT, 0);
    lv_obj_center(close_icon);
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

// Only used by load_wifi_credentials() as a last resort, before any network
// has ever been saved (saved_networks[] still empty) — see that function.
static const char *WIFI_SSID_FALLBACK = "OWAIS_4G";
static const char *WIFI_PASSWORD_FALLBACK = "0530331339";

// BLE (Wi-Fi provisioning) — confirmed disabled for real this time, not just
// a USB artifact: tested untethered on battery and it still corrupted the
// display. Worse than the original finding suggested, too — a forced
// periodic full-screen redraw (see loop()'s ble_active check) didn't help,
// and taps kept registering (the touch/LVGL side was fine) while the
// screen simply stopped updating — meaning the low-level display flush
// pipeline itself was wedged, not just a stale cached frame LVGL could be
// told to redraw. That's a driver/hardware-level issue (see
// LilyGoDispInterface.cpp's TE-interrupt-driven flush), not something
// fixable from application code here. WIFI_AP_PROVISIONING_ENABLED below
// is the replacement: same "change Wi-Fi from Settings" user flow, but
// using only the Wi-Fi radio (no BLE stack running at all), which doesn't
// hit this conflict.
static const bool BLE_PROVISIONING_ENABLED = false;

// Wi-Fi hotspot + captive portal — the replacement for BLE provisioning
// above. The watch briefly runs its own open Wi-Fi network; connecting a
// phone to it and opening any webpage (most phones pop this up
// automatically, like any other public Wi-Fi login page) shows a simple
// form to enter the real network's name/password. Feeds into the exact
// same pending_wifi_ssid/pending_wifi_pass/wifi_credentials_pending
// hand-off BLE provisioning used, so everything downstream of that
// (saving to NVS, reconnecting, notifying) is unchanged.
static const bool WIFI_AP_PROVISIONING_ENABLED = true;
// The suffix comes from this watch's own MAC address, so two watches being
// set up nearby never advertise the same network name. The AP itself stays
// open for compatibility with captive-portal detection; the fresh, on-screen
// three-digit code below protects the only sensitive action: saving Wi-Fi
// credentials to the watch.
#define WIFI_AP_SSID_PREFIX "Nabeeh_Setup"
#define WIFI_SETUP_PIN_DIGITS 3
static char wifi_ap_ssid[32] = "";
static char wifi_setup_pin[WIFI_SETUP_PIN_DIGITS + 1] = "";

static void prepare_wifi_setup_identity()
{
    // Must run after WiFi.mode(...) has enabled the STA interface. The last
    // two bytes are stable for this hardware and make a short, readable ID.
    if (wifi_ap_ssid[0] == '\0') {
        uint8_t mac[6] = {};
        WiFi.macAddress(mac);
        snprintf(wifi_ap_ssid, sizeof(wifi_ap_ssid), "%s-%02X%02X",
                 WIFI_AP_SSID_PREFIX, mac[4], mac[5]);
    }

    // A new code for every setup window: a code seen during an old setup
    // cannot be reused when the owner opens Wi-Fi setup again later.
    snprintf(wifi_setup_pin, sizeof(wifi_setup_pin), "%03u",
             (unsigned)(esp_random() % 1000));
}

static DNSServer portal_dns;
static WebServer portal_server(80);
static bool portal_active = false;
// Cross-task signal for the Settings-screen feedback label: network_task
// (core 0) sets this once the AP is actually up, the UI task (core 1)
// polls it on a timer and updates wifi_status_label — same "owning task
// writes, other task polls" pattern as wifi_credentials_pending elsewhere
// in this file, just for UI feedback instead of a credential hand-off.
static volatile bool portal_ready_for_ui = false;
static volatile bool wifi_setup_dismissed = false;
// The toast that shows the setup network name/PIN used to pop up on ANY
// screen the moment portal_ready_for_ui went true — including when setup
// mode kicked in on its own (e.g. a saved network's password stopped
// working and the watch fell back to broadcasting its own setup hotspot).
// That read as a random popup appearing while just opening Settings. It now
// only shows when the user actually tapped "تغيير الشبكة" — change_wifi_cb
// sets this true, update_wifi_status_display_cb checks it before showing the
// box, and it's cleared once the box hides again.
static volatile bool wifi_setup_shown_by_user = false;
// Guards against rapid repeated taps on a saved-network row: each tap
// disconnects and restarts WiFi.begin() (see connect_to_saved_network_cb),
// so tapping again before the previous attempt resolves just interrupts its
// handshake mid-flight — confirmed on a real device, logged as repeated
// "Reconnecting to newly provisioned..." lines seconds apart with
// AUTH_EXPIRE in between, which reads to the user as the watch randomly
// disconnecting/reconnecting from the phone. Set true the moment a tap is
// accepted, cleared once network_task's wifi_reconnect_pending resolves
// (success or failure) — see both branches of that block.
static volatile bool wifi_switch_in_progress = false;

// Set by network_task (core 0) right after a reconnect (BLE/AP reprovision or
// tapping a saved network) resolves to WL_CONNECTED — tells the UI task to
// rebuild the saved-networks list so the green "(متصلة الآن)" marker moves to
// the newly-connected network immediately, instead of only updating the next
// time the Wi-Fi settings screen happens to be re-opened.
static volatile bool wifi_saved_list_needs_refresh = false;

// اختبار مؤقت لتأثيرات مكتبة DRV2605 الاهتزازية (١٢٣ تأثير) — يُرسل عبر
// USB Serial بصيغة "V<رقم>\n" (مثلًا "V47\n")، تُعالج بـhandle_incoming_byte
// (أي مهمة) لكن الاهتزاز الفعلي ما يصير إلا بـloop() على مهمة الواجهة —
// نفس قاعدة الملف: DRV2605 على نفس ناقل I2C اللي عليه RTC/PMU، فأي استدعاء
// من مهمة الشبكة يخاطر بتجميد الناقل (صار فعليًا أثناء اختبار سابق).
static volatile int pending_vib_test_effect = 0; // 0 = لا يوجد طلب معلّق
static bool vib_test_rx_active = false;
static char vib_test_buf[8];
static size_t vib_test_len = 0;
// Set by the toast's close (X) button (UI task); network_task tears down
// whichever provisioning method is actually running (or about to start)
// and resets portal_ready_for_ui, same hand-off pattern as everything else
// crossing the two tasks in this file.
static volatile bool cancel_wifi_setup_requested = false;

#define BLE_PROVISIONING_DEVICE_NAME "Nabeeh-Watch-Setup"
#define BLE_WIFI_SERVICE_UUID        "b19c1e70-1fc7-4b2b-9f5b-8f2e6f2b1a01"
#define BLE_WIFI_CHAR_UUID           "b19c1e71-1fc7-4b2b-9f5b-8f2e6f2b1a01"
#define BLE_STATUS_CHAR_UUID         "b19c1e72-1fc7-4b2b-9f5b-8f2e6f2b1a01" // notify-only: 'K' = connected, 'F' = failed/timed out

static BLECharacteristic *status_characteristic = nullptr;

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

// Minimal styling inline (no external CSS) since this is served straight
// from flash to whatever browser the phone opens — kept deliberately small.
static const char PORTAL_PAGE_HEAD[] PROGMEM = R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Nabeeh Watch Wi-Fi Setup</title>
<style>
body{font-family:sans-serif;background:#0B0B1A;color:#fff;display:flex;flex-direction:column;align-items:center;padding:32px 16px;margin:0}
h1{font-size:20px;margin-bottom:4px}
p{color:#A0A0B8;font-size:14px;margin-top:0;text-align:center}
form{width:100%;max-width:320px;margin-top:24px}
label{display:block;font-size:13px;color:#A0A0B8;margin-bottom:4px}
input{width:100%;box-sizing:border-box;padding:10px;margin-bottom:16px;border-radius:8px;border:1px solid #333;background:#181059;color:#fff;font-size:16px}
.select-wrap{position:relative;margin-bottom:16px}
.select-wrap select{width:100%;box-sizing:border-box;padding:10px 32px 10px 10px;border-radius:8px;border:1px solid #333;background:#181059;color:#fff;font-size:16px;-webkit-appearance:none;-moz-appearance:none;appearance:none}
.select-wrap:after{content:'\25BE';position:absolute;right:12px;top:50%;transform:translateY(-50%);color:#A0A0B8;pointer-events:none;font-size:14px}
option{background:#181059;color:#fff}
button{width:100%;padding:12px;border-radius:8px;border:none;background:#181059;color:#fff;font-size:16px;font-weight:bold}
</style></head><body>
<h1>Nabeeh Watch</h1>
<p>Connect your watch to Wi-Fi</p>
)HTML";
static const char PORTAL_PAGE_TAIL[] PROGMEM = "</body></html>";

// Scanned-network dropdown + "Other" manual fallback (hidden networks, or
// if the scan hasn't found it for some reason). ssidManual is the only
// input actually named "ssid" — the <select> itself submits nothing; the
// inline script copies its chosen value into ssidManual right before
// submit, or leaves whatever the user typed there if they picked "Other".
// No external JS/CSS: this page is served entirely from the watch itself,
// so everything has to be inline.
static void handle_portal_root()
{
    String page = String(PORTAL_PAGE_HEAD) +
        "<form id=\"wifiForm\" method=\"POST\" action=\"/save\">"
        "<p>Enter the 3-digit setup code shown on your watch.</p>"
        "<label>Setup code</label>"
        "<input name=\"setup_pin\" inputmode=\"numeric\" pattern=\"[0-9]{3}\" maxlength=\"3\" "
        "placeholder=\"000\" autocomplete=\"one-time-code\" required>"
        "<label>Wi-Fi network</label>"
        "<div class=\"select-wrap\"><select id=\"ssidSelect\" disabled><option value=\"\">Scanning for networks…</option></select></div>"
        "<input type=\"text\" id=\"ssidManual\" name=\"ssid\" placeholder=\"Network name\" style=\"display:none\" "
        "maxlength=\"63\" autocapitalize=\"off\" autocorrect=\"off\" autocomplete=\"off\">"
        "<label>Wi-Fi password</label>"
        "<input name=\"password\" type=\"password\" maxlength=\"63\" autocomplete=\"off\">"
        "<button type=\"submit\">Connect</button>"
        "</form>"
        "<script>"
        "var OTHER='__other__';"
        "function syncManual(){"
        "  var sel=document.getElementById('ssidSelect');"
        "  document.getElementById('ssidManual').style.display=(sel.value===OTHER)?'block':'none';"
        "}"
        "function poll(){"
        "  fetch('/scan').then(function(r){return r.json();}).then(function(data){"
        "    if(data.status==='scanning'){setTimeout(poll,1500);return;}"
        "    var sel=document.getElementById('ssidSelect');"
        "    sel.innerHTML='';"
        "    if(!data.networks||data.networks.length===0){"
        "      var o=document.createElement('option');o.value='';o.textContent='No networks found nearby';sel.appendChild(o);"
        "    } else {"
        "      data.networks.forEach(function(name){"
        "        var o=document.createElement('option');o.value=name;o.textContent=name;sel.appendChild(o);"
        "      });"
        "    }"
        "    var other=document.createElement('option');other.value=OTHER;other.textContent='Other (type manually)';sel.appendChild(other);"
        "    sel.disabled=false;"
        "    syncManual();"
        "  }).catch(function(){setTimeout(poll,2000);});"
        "}"
        "document.addEventListener('DOMContentLoaded',function(){"
        "  poll();"
        "  document.getElementById('ssidSelect').addEventListener('change',syncManual);"
        "  document.getElementById('wifiForm').addEventListener('submit',function(e){"
        "    var sel=document.getElementById('ssidSelect');"
        "    var manual=document.getElementById('ssidManual');"
        "    if(sel.value!==OTHER){manual.value=sel.value;}"
        "    if(!manual.value){e.preventDefault();alert('Choose a network or type its name.');}"
        "  });"
        "});"
        "</script>" + String(PORTAL_PAGE_TAIL);
    portal_server.send(200, "text/html", page);
}

// Kicks off (or reads the result of) an async Wi-Fi scan. The page above
// polls this every ~1.5s while "scanning", then fills the dropdown once it
// gets "done". scanDelete() frees the result and resets scanComplete()
// back to "not started", so the very next /scan call (next page load, or
// the phone's poll loop if it ever calls this again) starts a fresh scan
// rather than reusing stale results indefinitely.
static void handle_portal_scan()
{
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
        portal_server.send(200, "application/json", "{\"status\":\"scanning\"}");
        return;
    }
    if (n == WIFI_SCAN_FAILED) {
        WiFi.scanNetworks(true, false); // async; don't bother listing hidden networks, there's no name to show anyway
        portal_server.send(200, "application/json", "{\"status\":\"scanning\"}");
        return;
    }

    // Sort strongest-signal first, then list each SSID only once — a mesh
    // system or repeater broadcasting the same network name from multiple
    // access points would otherwise show up as several identical entries.
    const int MAX_NETWORKS = 64;
    int count = n < MAX_NETWORKS ? n : MAX_NETWORKS;
    int order[MAX_NETWORKS];
    for (int i = 0; i < count; i++) order[i] = i;
    for (int i = 1; i < count; i++) {
        int key = order[i], j = i - 1;
        while (j >= 0 && WiFi.RSSI(order[j]) < WiFi.RSSI(key)) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }

    String json = "{\"status\":\"done\",\"networks\":[";
    String seen[MAX_NETWORKS];
    int seen_count = 0;
    bool first_entry = true;
    for (int k = 0; k < count; k++) {
        String ssid = WiFi.SSID(order[k]);
        if (ssid.length() == 0) continue; // hidden network — nothing to show in a dropdown
        bool dup = false;
        for (int s = 0; s < seen_count; s++) {
            if (seen[s] == ssid) { dup = true; break; }
        }
        if (dup) continue;
        seen[seen_count++] = ssid;

        String escaped = ssid;
        escaped.replace("\\", "\\\\");
        escaped.replace("\"", "\\\"");
        if (!first_entry) json += ",";
        first_entry = false;
        json += "\"" + escaped + "\"";
    }
    json += "]}";
    WiFi.scanDelete();
    portal_server.send(200, "application/json", json);
}

// Reuses the exact same pending_wifi_ssid/pending_wifi_pass/
// wifi_credentials_pending hand-off that WifiProvisionCallbacks::onWrite()
// (BLE path, above) used — network_task's loop doesn't need to know or
// care which frontend produced these, it saves-and-reconnects the same way
// either way.
static void handle_portal_save()
{
    String setup_pin = portal_server.arg("setup_pin");
    String ssid = portal_server.arg("ssid");
    String pass = portal_server.arg("password");

    if (setup_pin != wifi_setup_pin) {
        portal_server.send(403, "text/html",
            String(PORTAL_PAGE_HEAD) +
            "<p style=\"color:#ff8080\">Incorrect setup code. Check the watch and try again.</p>"
            "<button onclick=\"history.back()\">Go back</button>" +
            String(PORTAL_PAGE_TAIL));
        Serial.println("Wi-Fi AP provisioning: rejected credentials with an incorrect setup code.");
        return;
    }

    if (ssid.length() == 0 || ssid.length() >= sizeof(pending_wifi_ssid) || pass.length() >= sizeof(pending_wifi_pass)) {
        portal_server.send(400, "text/html",
            String(PORTAL_PAGE_HEAD) +
            "<p style=\"color:#ff8080\">Network name missing or too long — go back and try again.</p>" +
            String(PORTAL_PAGE_TAIL));
        return;
    }

    strncpy(pending_wifi_ssid, ssid.c_str(), sizeof(pending_wifi_ssid) - 1);
    pending_wifi_ssid[sizeof(pending_wifi_ssid) - 1] = '\0';
    strncpy(pending_wifi_pass, pass.c_str(), sizeof(pending_wifi_pass) - 1);
    pending_wifi_pass[sizeof(pending_wifi_pass) - 1] = '\0';
    wifi_credentials_pending = true;

    portal_server.send(200, "text/html",
        String(PORTAL_PAGE_HEAD) +
        "<p>Saved. Your watch is reconnecting now — this page can be closed.</p>" +
        String(PORTAL_PAGE_TAIL));
    Serial.printf("Wi-Fi AP provisioning: received new SSID '%s'\n", pending_wifi_ssid);
}

static void start_wifi_ap_provisioning()
{
    if (portal_active) return;
    wifi_setup_dismissed = false;
    // AP_STA, not just AP: scanning for nearby networks (for the dropdown)
    // needs the station radio active too, alongside the AP the phone
    // connects to.
    WiFi.mode(WIFI_AP_STA);
    // بدون هذا، جهة STA تفضل تحاول تتصل تلقائيًا بآخر شبكة كانت تحاولها قبل
    // دخول وضع الإعداد (زي WIFI_SSID_FALLBACK لو ما فيه شبكات محفوظة) —
    // محاولات متكررة كل ~٢ ثانية تزاحم راديو الواي فاي مع بث شبكة الإعداد
    // (AP) نفسها، فتخليه غير مستقر أو يختفي من قائمة شبكات الجوال. جهة STA
    // هنا لازم تفضل خاملة تمامًا طول وضع الإعداد — تُستخدم بس للمسح عند
    // الطلب (WiFi.scanNetworks تحت)، مو للاتصال.
    WiFi.disconnect();
    WiFi.setAutoReconnect(false);
    prepare_wifi_setup_identity();
    WiFi.softAP(wifi_ap_ssid); // open network — brief, setup-only; portal requires the on-screen PIN
    IPAddress ap_ip = WiFi.softAPIP(); // normally 192.168.4.1
    WiFi.scanNetworks(true, false); // kick off async now, so it's likely already done by the time the phone loads the page

    portal_dns.start(53, "*", ap_ip); // every DNS lookup from a connected phone resolves here

    portal_server.on("/", HTTP_GET, handle_portal_root);
    portal_server.on("/scan", HTTP_GET, handle_portal_scan);
    portal_server.on("/save", HTTP_POST, handle_portal_save);
    // OS captive-portal probes: answering these with our own page (instead
    // of the "you're already online" response they expect) is what makes
    // the setup page pop up automatically, the same way a hotel/airport
    // Wi-Fi login page does, instead of the user having to manually open a
    // browser and type 192.168.4.1 themselves.
    portal_server.on("/generate_204", HTTP_GET, handle_portal_root);        // Android
    portal_server.on("/gen_204", HTTP_GET, handle_portal_root);             // Android (older)
    portal_server.on("/hotspot-detect.html", HTTP_GET, handle_portal_root); // iOS/macOS
    portal_server.on("/library/test/success.html", HTTP_GET, handle_portal_root); // iOS (older)
    portal_server.on("/ncsi.txt", HTTP_GET, handle_portal_root);            // Windows
    portal_server.on("/connecttest.txt", HTTP_GET, handle_portal_root);     // Windows
    portal_server.on("/fwlink", HTTP_GET, handle_portal_root);              // Windows
    portal_server.onNotFound(handle_portal_root); // anything else: same page — DNS already sent every domain here

    portal_server.begin();
    portal_active = true;
    portal_ready_for_ui = true; // tells the Settings screen's status label to show the network name
    Serial.printf("Wi-Fi AP provisioning: advertising as '%s' (open network, PIN %s), setup page at http://%s/\n",
                  wifi_ap_ssid, wifi_setup_pin, ap_ip.toString().c_str());
}

static void stop_wifi_ap_provisioning()
{
    if (!portal_active) return;
    portal_ready_for_ui = false; // hides the status label again
    portal_server.stop();
    portal_dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    portal_active = false;
    Serial.println("Wi-Fi AP provisioning stopped.");
}

// Settings screen button: only sets a flag here (UI task) — network_task
// picks it up and actually starts BLE/AP provisioning on its own task,
// same hand-off pattern as everything else that touches shared hardware in
// this file. Name kept from the BLE-only days; it now triggers whichever
// of BLE_PROVISIONING_ENABLED / WIFI_AP_PROVISIONING_ENABLED is on.
//
// wifi_status_label is updated directly here (immediate, since this
// callback already runs on the UI task) so tapping the button always shows
// *something* right away — bringing the AP up can take a second or two,
// and with no feedback at all a tap that "did nothing" reads as broken and
// invites mashing the button. update_wifi_status_display_cb (a timer, see
// setup()) takes over from here once network_task actually has the
// network ready, via portal_ready_for_ui.
static void change_wifi_cb(lv_event_t *e)
{
    if (wifi_status_box) {
        lv_obj_set_style_bg_color(wifi_status_box, COLOR_PRIMARY, 0);
        lv_obj_set_style_bg_opa(wifi_status_box, LV_OPA_COVER, 0);
        lv_label_set_text(wifi_status_label, "جارٍ تجهيز شبكة الإعداد...");
        lv_obj_remove_flag(wifi_status_box, LV_OBJ_FLAG_HIDDEN);
    }
    wifi_setup_shown_by_user = true;
    // لازم نصفّرها هنا، مو نعتمد بس على start_wifi_ap_provisioning() تحت —
    // تلك الدالة ترجع فورًا بدون ما تصفّرها لو الشبكة شغّالة أصلًا
    // (portal_active)، وهذا بالضبط اللي كان يصير: بعد أول ضغطة X، هذي القيمة
    // تفضل عالقة true للأبد وتمنع التنبيه من عرض اسم الشبكة/الرمز أي مرة
    // بعدها — مؤكد بتشخيص فعلي على جهاز حقيقي.
    wifi_setup_dismissed = false;
    ble_restart_requested = true;
}

// صف شبكة محفوظة انضغط بقائمة شاشة إعدادات الواي فاي. يمرّ بنفس التسليم
// (pending_wifi_ssid/pending_wifi_pass/wifi_credentials_pending) اللي يستخدمه
// إعداد BLE/نقطة الوصول — network_task هو من يقطع الاتصال الحالي ويتصل
// بالشبكة الجديدة، ونفس المكان يعيد استدعاء add_or_update_saved_network()
// فتصير هذي الشبكة تلقائيًا بالمقدمة (الأحدث استخدامًا).
static void connect_to_saved_network_cb(lv_event_t *e)
{
    if (wifi_switch_in_progress) return; // تجاهل الضغطة — فيه محاولة سابقة لسا ما خلصت
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= saved_network_count) return;

    wifi_switch_in_progress = true;
    strncpy(pending_wifi_ssid, saved_networks[idx].ssid, sizeof(pending_wifi_ssid) - 1);
    pending_wifi_ssid[sizeof(pending_wifi_ssid) - 1] = '\0';
    strncpy(pending_wifi_pass, saved_networks[idx].pass, sizeof(pending_wifi_pass) - 1);
    pending_wifi_pass[sizeof(pending_wifi_pass) - 1] = '\0';
    wifi_credentials_pending = true;

    if (wifi_status_box) {
        lv_obj_set_style_bg_color(wifi_status_box, COLOR_PRIMARY, 0);
        lv_obj_set_style_bg_opa(wifi_status_box, LV_OPA_COVER, 0);
        char buf[96];
        snprintf(buf, sizeof(buf), "جارٍ الاتصال بـ%s...", saved_networks[idx].ssid);
        lv_label_set_text(wifi_status_label, buf);
        lv_obj_remove_flag(wifi_status_box, LV_OBJ_FLAG_HIDDEN);
        // ما فيه إشعار "نجح" مخصص لهذا المسار (بخلاف تدفق نقطة الوصول اللي
        // يراقبه update_wifi_status_display_cb عبر portal_ready_for_ui) —
        // نخفيه تلقائيًا بعد مهلة معقولة بدل ما يعلّق على الشاشة للأبد.
        lv_timer_t *t = lv_timer_create(
            [](lv_timer_t *t) {
                if (wifi_status_box) lv_obj_add_flag(wifi_status_box, LV_OBJ_FLAG_HIDDEN);
            },
            4000, NULL);
        lv_timer_set_repeat_count(t, 1);
    }
}

// The toast's close (X) button — hides it immediately here (UI task) and
// asks network_task to actually tear down whatever's running/starting, so
// canceling doesn't leave the setup hotspot broadcasting in the background
// with nobody watching for it to finish.
static void cancel_wifi_setup_cb(lv_event_t *e)
{
    if (wifi_status_box) {
        lv_obj_add_flag(wifi_status_box, LV_OBJ_FLAG_HIDDEN);
    }
    wifi_setup_dismissed = true;
    wifi_setup_shown_by_user = false;
    cancel_wifi_setup_requested = true;
}

// Polls portal_ready_for_ui (set by network_task, core 0) and reflects it
// on the Settings screen — only touches LVGL objects here, on the UI task,
// same rule as everywhere else in this file that crosses the two tasks.
static void update_wifi_status_display_cb(lv_timer_t *t)
{
    static bool was_ready = false;
    static bool last_dbg_ready = false, last_dbg_shown = false, last_dbg_dismissed = false;
    if (portal_ready_for_ui != last_dbg_ready || wifi_setup_shown_by_user != last_dbg_shown || wifi_setup_dismissed != last_dbg_dismissed) {
        Serial.printf("DBG toast state: portal_ready=%d shown_by_user=%d dismissed=%d was_ready=%d\n",
                      portal_ready_for_ui, wifi_setup_shown_by_user, wifi_setup_dismissed, was_ready);
        last_dbg_ready = portal_ready_for_ui;
        last_dbg_shown = wifi_setup_shown_by_user;
        last_dbg_dismissed = wifi_setup_dismissed;
    }
    if (wifi_saved_list_needs_refresh) {
        wifi_saved_list_needs_refresh = false;
        refresh_wifi_saved_list(); // no-op if wifi_saved_list is NULL (settings screen not built yet)
    }

    if (!wifi_status_box) return; // Settings screen not built yet — nothing to update

    if (portal_ready_for_ui && wifi_setup_shown_by_user && !wifi_setup_dismissed && !was_ready) {
        char buf[96];
        snprintf(buf, sizeof(buf), "اتصل من جوالك بشبكة:\n%s\nرمز الإعداد: %s",
                 wifi_ap_ssid, wifi_setup_pin);
        lv_label_set_text(wifi_status_label, buf); // same COLOR_PRIMARY box change_wifi_cb() already showed — only the text changes, no color swap
        lv_obj_remove_flag(wifi_status_box, LV_OBJ_FLAG_HIDDEN);
    } else if ((!portal_ready_for_ui || wifi_setup_dismissed || !wifi_setup_shown_by_user) && was_ready) {
        lv_label_set_text(wifi_status_label, "");
        lv_obj_add_flag(wifi_status_box, LV_OBJ_FLAG_HIDDEN);
        wifi_setup_shown_by_user = false;
    }
    was_ready = portal_ready_for_ui && wifi_setup_shown_by_user;
}

// يجرب آخر شبكة استُخدمت (saved_networks[0] — انظر add_or_update_saved_network)
// أولًا، ولا يرجع للقيمة الثابتة WIFI_SSID_FALLBACK/WIFI_PASSWORD_FALLBACK إلا
// لو القائمة فاضية تمامًا (أول إقلاع قبل أي إعداد على الإطلاق).
static void load_wifi_credentials(char *ssid_out, size_t ssid_len, char *pass_out, size_t pass_len)
{
    load_saved_networks();
    if (saved_network_count > 0) {
        strncpy(ssid_out, saved_networks[0].ssid, ssid_len - 1);
        ssid_out[ssid_len - 1] = '\0';
        strncpy(pass_out, saved_networks[0].pass, pass_len - 1);
        pass_out[pass_len - 1] = '\0';
        return;
    }

    strncpy(ssid_out, WIFI_SSID_FALLBACK, ssid_len - 1);
    ssid_out[ssid_len - 1] = '\0';
    strncpy(pass_out, WIFI_PASSWORD_FALLBACK, pass_len - 1);
    pass_out[pass_len - 1] = '\0';
}

// عند الإقلاع: يختار أحدث شبكة محفوظة موجودة فعلًا بالمدى الحين، بدل ما يجرب
// saved_networks[0] بس. بدون هذا، لو آخر شبكة مطفية (راوتر متنقل مثلًا) الساعة
// تدخل وضع الإعداد مباشرة حتى لو فيه شبكة محفوظة ثانية شغالة جنبها. لو ولا
// وحدة ظهرت بالمسح (شبكة مخفية مثلًا) يبقى الاختيار الأصلي زي ما هو.
static void pick_boot_network(char *ssid_out, size_t ssid_len, char *pass_out, size_t pass_len)
{
    if (saved_network_count <= 1) return;
    int n = WiFi.scanNetworks();
    for (int i = 0; i < saved_network_count; i++) {
        for (int s = 0; s < n; s++) {
            if (WiFi.SSID(s) != saved_networks[i].ssid) continue;
            strncpy(ssid_out, saved_networks[i].ssid, ssid_len - 1);
            ssid_out[ssid_len - 1] = '\0';
            strncpy(pass_out, saved_networks[i].pass, pass_len - 1);
            pass_out[pass_len - 1] = '\0';
            WiFi.scanDelete();
            Serial.printf("Boot: saved network '%s' is in range — using it.\n", ssid_out);
            return;
        }
    }
    WiFi.scanDelete();
    Serial.println("Boot: no saved network found in scan — trying the most recent one anyway.");
}

// نحفظ نص الجدول كما وصل (مو المصفوفة المفكوكة): أبسط، ويخلي إعادة التحميل
// بعد إعادة التشغيل تمر بنفس دالة الفك ونفس التحقق بالضبط.
static void save_reminders_payload(const char *payload)
{
    wifi_prefs.begin("nabeeh", false);
    wifi_prefs.putString("reminders", payload ? payload : "");
    wifi_prefs.end();
}

static void load_reminders_from_prefs()
{
    wifi_prefs.begin("nabeeh", true); // read-only
    String saved = wifi_prefs.getString("reminders", "");
    wifi_prefs.end();
    Serial.printf("Reminders: المحفوظ بالذاكرة = \"%s\"\n", saved.c_str());
    int n = parse_reminder_payload(saved.c_str());
    Serial.printf("Reminders: حُمّل %d تذكير من الذاكرة\n", n < 0 ? 0 : n);
}

// تفريغ الحالة كاملة بضغطة زر — '?' بالسيريال (أو من التطبيق). هذا هو المكان
// الوحيد اللي يحتاجه أي سؤال عن "ليش التذكير ما اشتغل": وقت الساعة، اليوم
// المحسوب، كل تذكير مخزّن بحالته، وآخر مرة انطلق فيها تذكير.
static void dump_reminder_state(const char *source)
{
    const RtcSnapshot now = rtc_snapshot;
    int today = now.valid ? weekday_iso(now.year, now.month, now.day) : 0;

    wifi_prefs.begin("nabeeh", true);
    String saved = wifi_prefs.getString("reminders", "");
    wifi_prefs.end();

    Serial.printf("\n===== حالة التذكيرات (%s) =====\n", source);
    if (now.valid) {
        Serial.printf("ساعة الجهاز : %04d-%02d-%02d %02d:%02d  (يوم الأسبوع %d، bit=0x%02X)\n",
                      now.year, now.month, now.day, now.hour, now.minute,
                      today, 1 << (today - 1));
    } else {
        Serial.println("ساعة الجهاز : لسا ما قُرئت (مؤقّت الساعة ما اشتغل بعد)");
    }
    Serial.printf("بالذاكرة    : \"%s\"\n", saved.c_str());
    Serial.printf("بالرام      : %d تذكير\n", watch_reminder_count);
    for (int i = 0; i < watch_reminder_count; i++) {
        WatchReminder &r = watch_reminders[i];
        Serial.printf("  [%d] %02d:%02d mask=0x%02X نمط=%c شدة=%c مرة_وحدة=%d اشتغل=%d اسم=\"%s\"\n",
                      i, r.hour, r.minute, r.days_mask, r.pattern, r.intensity,
                      r.once ? 1 : 0, r.spent ? 1 : 0, r.label);
    }
    if (last_reminder_dispatch_ms == 0) {
        Serial.println("آخر تذكير  : ما انطلق أي تذكير منذ التشغيل");
    } else {
        Serial.printf("آخر تذكير  : قبل %lu ثانية (نافذة كتم التكرار %lu ثانية)\n",
                      (millis() - last_reminder_dispatch_ms) / 1000,
                      REMINDER_DUPLICATE_WINDOW_MS / 1000);
    }
    Serial.printf("اتصال الجوال: %s\n", phone_connected ? "متصل" : "غير متصل");
    Serial.println("=================================\n");
}
static const uint16_t TCP_PORT = 3333;
static const char *MDNS_HOSTNAME = "nabeeh-watch";
static const size_t CHUNK_SIZE = 1024; // 512 samples @ 16-bit = ~32ms of audio per chunk

// The T-Watch S3's built-in PDM mic reads quiet by default — normal-distance
// and close-up sounds (e.g. a knock right at the wrist) come in as low-
// amplitude PCM that the classifier model sees as near-silence. LilyGoLib's
// mic wrapper doesn't expose a hardware gain register for the PDM peripheral,
// so we amplify digitally here instead: multiply every 16-bit sample by
// MIC_SOFTWARE_GAIN and hard-clip to the int16 range (rather than letting it
// wrap around, which would turn loud sounds into harsh digital noise).
// Start around 4x and raise/lower after listening to captured audio — too
// high and the noise floor gets amplified into audible hiss and true loud
// sounds clip; too low and quiet/near sounds are still missed.
#define MIC_SOFTWARE_GAIN 6.0f
// Peak-level testing at gain=4 and gain=10 both showed a suspiciously
// steady "floor" that scaled exactly with the gain multiplier (~20% at 4x,
// ~50% at 10x) even during total silence/distance — a real noise floor
// fluctuates sample to sample, it doesn't sit dead flat. That's the
// signature of a constant DC bias in the PDM demodulator's output, not
// random self-noise, and gain alone can never separate real signal from a
// bias that scales right along with it. A one-pole DC-blocking high-pass
// filter (standard technique: y[n] = x[n] - x[n-1] + R*y[n-1]) removes a
// constant offset entirely while passing speech/crying-range frequencies
// through basically unaffected, so gain afterward amplifies only the real,
// varying part of the signal.
static void remove_dc_offset(int16_t *samples, size_t sample_count)
{
    static float prev_x = 0.0f;
    static float prev_y = 0.0f;
    const float R = 0.995f; // pole near 1 -> very low cutoff, keeps everything above a few Hz
    for (size_t i = 0; i < sample_count; i++) {
        float x = (float)samples[i];
        float y = x - prev_x + R * prev_y;
        prev_x = x;
        prev_y = y;
        if (y > 32767.0f) y = 32767.0f;
        else if (y < -32768.0f) y = -32768.0f;
        samples[i] = (int16_t)y;
    }
}

static void apply_mic_gain(uint8_t *buf, size_t byte_count, float gain)
{
    int16_t *samples = (int16_t *)buf;
    size_t sample_count = byte_count / sizeof(int16_t);
    remove_dc_offset(samples, sample_count);
    for (size_t i = 0; i < sample_count; i++) {
        int32_t amplified = (int32_t)(samples[i] * gain);
        if (amplified > INT16_MAX) amplified = INT16_MAX;
        else if (amplified < INT16_MIN) amplified = INT16_MIN;
        samples[i] = (int16_t)amplified;
    }
}

WiFiServer server(TCP_PORT);
WiFiClient client;
// A new connection waits here until it proves it knows device_key (see the
// AUTH handling around server.available() below); only then is it promoted to
// `client`. So `client` is always an authenticated session, and nothing an
// unauthenticated connection sends is ever parsed as a command.
static WiFiClient pending_client;
enum PendingStage { PENDING_AWAIT_PAIR, PENDING_AWAIT_AUTH };
static PendingStage pending_stage = PENDING_AWAIT_AUTH;
static bool pending_is_pairing = false;
static uint8_t pending_key[DEVICE_KEY_LEN]; // device_key, or the freshly agreed key while pairing (saved only once AUTH proves it)
static uint8_t pair_watch_pub[32];
static uint8_t auth_nonce[16];
static char auth_line_buf[80]; // "PAIR,"/"AUTH," + 64 hex chars, the longest line this parser needs
static size_t auth_line_len = 0;
static unsigned long auth_challenge_sent_ms = 0; // start of whichever handshake step is currently waiting on the phone
#define AUTH_TIMEOUT_MS 5000 // per step — an unauthenticated connection that doesn't respond in time gets dropped

static void send_auth_challenge()
{
    esp_fill_random(auth_nonce, sizeof(auth_nonce));
    char nonce_hex[sizeof(auth_nonce) * 2 + 1];
    bytes_to_hex(auth_nonce, sizeof(auth_nonce), nonce_hex);
    size_t sent = pending_client.printf("AUTH,%s\n", nonce_hex);
    pending_stage = PENDING_AWAIT_AUTH;
    auth_challenge_sent_ms = millis();
    Serial.printf("DBG AUTH challenge sent: %u bytes, fd=%d, connected=%d\n",
                  (unsigned)sent, pending_client.fd(), (int)pending_client.connected());
}
static bool mdns_started = false;

static void ensure_mdns_service()
{
    if (WiFi.status() != WL_CONNECTED || mdns_started) return;

    if (MDNS.begin(MDNS_HOSTNAME)) {
        MDNS.addService("nabeeh", "tcp", TCP_PORT);
        mdns_started = true;
        Serial.printf("mDNS service ready: %s.local (_nabeeh._tcp, port %u)\n",
                      MDNS_HOSTNAME, TCP_PORT);
    } else {
        Serial.println("mDNS startup failed; phone discovery is unavailable.");
    }
}

static bool was_connected = false;
// The phone app doesn't hold one socket open the whole time it's "connected"
// — it connects briefly to send an 'I' status/battery poll, gets its reply,
// and disconnects again (confirmed from a real serial log: "Client
// connected." -> "Info request (client): I,1,86,0" -> "Client disconnected."
// within ~11ms). Driving the dot off "is a socket open right now" made it
// flicker true for a few ms per poll and false the rest of the time, which
// in practice just always read as "not connected" except while a long-lived
// connection (like mic streaming) was actually open. Tracking "seen within
// the last few seconds" instead — same idea as a phone showing a paired
// earbud as connected between its own periodic heartbeats — fixes that.
static unsigned long last_client_seen_millis = 0;
// 15s is a temporary safety margin for today's connect-poll-disconnect
// pattern, not a real fix — once the app holds one connection open instead
// of reconnecting to poll, this stops mattering almost entirely (see the
// comment above last_client_seen_millis).
#define CLIENT_ACTIVITY_TIMEOUT_MS 15000
#define WIFI_SCAN_TIMEOUT_MS 8000 // safety cap on the Wi-Fi settings screen's "nearby networks" scan — see its use in network_task
static volatile bool streaming = false; // written on both tasks (Serial on UI task, client on network task), read on network task; a plain flag is fine for this single on/off signal
// Non-blocking audio send — see the streaming block at the end of network_task.
static unsigned long last_audio_send_ok_ms = 0;
static unsigned long audio_drop_count = 0;
#define AUDIO_SEND_STALL_MS 3000

static bool socket_writable_now(int fd)
{
    if (fd < 0) return false;
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv = {0, 0};
    return select(fd + 1, NULL, &set, NULL, &tv) > 0;
}

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

// استقبال جدول التذكيرات: على عكس '#' (طوله ثابت ٤ بايت)، هذا الأمر نصّي
// بطول متغيّر، فنجمع البايتات لين '\n'. ١٦ تذكير × ~٢٠ حرف = ~٣٢٠ بايت،
// والمخزن ضعف هذا بهامش مريح.
static bool reminder_rx_active = false;
static char reminder_rx_buf[1600]; // ١٦ تذكير × (~٢٠ بايت أرقام + اسم لين ٤٧ بايت) + هامش
static size_t reminder_rx_len = 0;
static unsigned long reminder_rx_started = 0;
#define REMINDER_RX_TIMEOUT_MS 3000
#define WIFI_RECONNECT_TIMEOUT_MS 15000
#define RECONNECT_BACKOFF_MAX_MS 30000
static uint8_t audio_buf[CHUNK_SIZE];
static unsigned long audio_chunks_sent = 0;
static unsigned long audio_bytes_sent = 0;
static bool audio_read_started = false;
static bool microphone_active = true; // instance.begin() initializes it during setup()

static bool start_microphone()
{
    if (microphone_active) return true;

    instance.mic.setPinsPdmRx(MIC_SCK, MIC_DAT);
    microphone_active = instance.mic.begin(
        I2S_MODE_PDM_RX,
        16000,
        I2S_DATA_BIT_WIDTH_16BIT,
        I2S_SLOT_MODE_MONO,
        I2S_STD_SLOT_LEFT);
    Serial.printf("microphone started: %s (PCM16 mono 16000Hz)\n",
                  microphone_active ? "yes" : "no");
    return microphone_active;
}

static void stop_microphone()
{
    if (!microphone_active) return;
    instance.mic.end();
    microphone_active = false;
    Serial.println("microphone stopped");
}

// "last sync" = how long the *current* connection has been up (counts up
// while connected); once disconnected, it switches to how long ago that
// connection ended instead. phone_connected (set below, in this same task,
// right alongside was_connected) now tracks the same real client state.
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
    if (vib_test_rx_active) {
        if (c == '\n' || c == '\r') {
            vib_test_buf[vib_test_len] = '\0';
            vib_test_rx_active = false;
            int effect = atoi(vib_test_buf);
            if (effect >= 1 && effect <= 123) {
                pending_vib_test_effect = effect; // loop() على مهمة الواجهة هو من يشغّله فعليًا
            } else {
                Serial.printf("Vib test (%s): رقم غير صالح \"%s\" — لازم يكون بين 1 و123\n", source, vib_test_buf);
            }
            return;
        } else if (isdigit((unsigned char)c) && vib_test_len < sizeof(vib_test_buf) - 1) {
            vib_test_buf[vib_test_len++] = c;
            return;
        } else {
            vib_test_rx_active = false; // بايت غلط — نلغي ونكمل تحت كأمر عادي
        }
    }
    if (c == 'v' || c == 'V') {
        vib_test_rx_active = true;
        vib_test_len = 0;
        return;
    }

    if (reminder_rx_active) {
        if (millis() - reminder_rx_started > REMINDER_RX_TIMEOUT_MS) {
            Serial.printf("Reminders (%s): انتهت مهلة الاستقبال — إلغاء\n", source);
            reminder_rx_active = false;
            // نكمل لتحت: البايت الحالي يُعامل كأمر عادي بدل ما يضيع.
        } else if (c == '\n' || c == '\r') {
            reminder_rx_buf[reminder_rx_len] = '\0';
            reminder_rx_active = false;
            Serial.printf("Reminders (%s): وصل \"%s\"\n", source, reminder_rx_buf);
            int n = parse_reminder_payload(reminder_rx_buf);
            if (n >= 0) {
                save_reminders_payload(reminder_rx_buf); // ما نحفظ رسالة مرفوضة
            } else {
                n = watch_reminder_count; // الردّ يعكس الجدول الفعلي، مو الرسالة المرفوضة
            }
            Serial.printf("Reminders (%s): الجدول الآن فيه %d تذكير\n", source, n);
            if (strcmp(source, "client") == 0 && client && client.connected()) {
                char ack[16];
                snprintf(ack, sizeof(ack), "R,%d\n", n);
                client.print(ack);
            }
            return;
        } else if (reminder_rx_len < sizeof(reminder_rx_buf) - 1) {
            reminder_rx_buf[reminder_rx_len++] = c;
            return;
        } else {
            Serial.printf("Reminders (%s): الجدول أطول من المخزن — إلغاء\n", source);
            reminder_rx_active = false;
        }
    }

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
        if (!client || !client.connected()) {
            Serial.printf("Streaming request (%s) ignored: TCP client is not connected.\n", source);
            streaming = false;
            return;
        }
        if (streaming) {
            Serial.printf("command received: %c (ignored; streaming already active)\n", c);
            return;
        }
        Serial.printf("command received: %c\n", c);
        if (!start_microphone()) {
            Serial.println("streaming not started: microphone initialization failed");
            streaming = false;
            return;
        }
        streaming = true;
        last_audio_send_ok_ms = millis(); // the 3s "phone gone" window starts now, not at boot
        audio_drop_count = 0;
        audio_chunks_sent = 0;
        audio_bytes_sent = 0;
        audio_read_started = false;
        Serial.printf("Streaming started (%s): mic=PCM16 mono 16000Hz, TCP port=%u.\n",
                      source, TCP_PORT);
    } else if (c == 's' || c == 'S') {
        Serial.printf("command received: %c\n", c);
        streaming = false;
        stop_microphone();
        Serial.printf("Streaming stopped (%s): sent %lu chunks / %lu bytes; TCP server remains active.\n",
                      source, audio_chunks_sent, audio_bytes_sent);
    } else if (c == 'i' || c == 'I') {
        Serial.printf("command received: %c\n", c);
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
    } else if (c == '@') {
        Serial.printf("Reminders (%s): بدأ استقبال جدول جديد\n", source);
        reminder_rx_active = true;
        reminder_rx_len = 0;
        reminder_rx_started = millis();
    } else if (c == '?') {
        dump_reminder_state(source);
    } else if (c == '!') {
        // إطلاق تذكير تجريبي فورًا، بدون انتظار الساعة — يفصل "مسار التنبيه
        // نفسه" عن "منطق مطابقة الوقت" وقت التشخيص. يتخطّى كتم التكرار عن
        // قصد عشان تقدر تجربه مرات متتالية.
        last_reminder_dispatch_ms = 0;
        char pat = watch_reminder_count > 0 ? watch_reminders[0].pattern : '1';
        char inten = watch_reminder_count > 0 ? watch_reminders[0].intensity : '1';
        const char *lbl = watch_reminder_count > 0 ? watch_reminders[0].label : "";
        Serial.printf("Reminders (%s): إطلاق تجريبي (نمط=%c شدة=%c اسم=\"%s\")\n",
                      source, pat, inten, lbl);
        handle_result_code('T', pat, inten, source, lbl);
    }
}

// Correct the watch's hardware RTC using real network time, whenever Wi-Fi is
// up. The RTC (PCF8563) is only ever seeded once, from a hardcoded
// build-time value (see the one-time seed in setup() below) — over days/
// weeks its own crystal drifts (a few seconds a day is normal for this kind
// of RTC chip), which is exactly why the on-screen clock slowly runs ahead
// the longer the watch goes without a fresh sync. NTP fixes that for good
// as long as the watch has internet-connected Wi-Fi at least occasionally.
// Safe to call repeatedly (e.g. right after every (re)connect, and
// periodically after that — see network_task's main loop).
// Anything at or after 2025-01-01 00:00:00 UTC can only have come from a real
// NTP reply, never from the zeroed clock we start from below. Used as the
// "did NTP actually answer yet?" test — see the long comment in the function.
#define NTP_PLAUSIBLE_EPOCH 1735689600L

static bool sync_rtc_from_ntp()
{
    if (WiFi.status() != WL_CONNECTED) return false;

    // Why this doesn't just call getLocalTime() and trust it:
    //
    // The watch's system clock is already seeded from the hardware RTC early
    // in boot (instance.begin()), so it is "set" long before any NTP packet
    // arrives. getLocalTime() only checks that the year looks sane (>2016) —
    // it can't tell an RTC-seeded time from an NTP-corrected one — so it
    // returned instantly with the RTC's own (already wrong) time. We then
    // added +3h to that and wrote it straight back into the RTC. Every boot
    // therefore pushed the clock three hours further ahead instead of
    // correcting it: 20:33 -> 23:41 -> 02:51 across three flashes, which is
    // exactly the compounding this loop is here to stop.
    //
    // Fix: wipe the system clock to epoch 0 first, so any plausible modern
    // timestamp appearing afterwards can ONLY have come from the network,
    // then poll for one. Safe to zero because nothing else on the watch reads
    // the system clock — the battery-backed RTC is the only clock the UI and
    // everything else use, and it is left untouched unless a real NTP time
    // arrives below.
    struct timeval epoch_zero = {};
    settimeofday(&epoch_zero, nullptr);

    configTime(0, 0, "pool.ntp.org", "time.google.com"); // plain UTC; the Riyadh offset is applied once, manually, below

    time_t utc_epoch = 0;
    unsigned long wait_start = millis();
    while (millis() - wait_start < 10000) { // NTP's first reply can take a few seconds on a cold DNS cache
        utc_epoch = time(nullptr);
        if (utc_epoch >= NTP_PLAUSIBLE_EPOCH) break;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (utc_epoch < NTP_PLAUSIBLE_EPOCH) {
        Serial.println("NTP time sync failed (no reply within 10s) — RTC left unchanged.");
        return false;
    }

    time_t riyadh_epoch = utc_epoch + 3 * 3600; // Asia/Riyadh, UTC+3, no DST — the one and only offset applied anywhere in this function
    struct tm riyadh_tm;
    gmtime_r(&riyadh_epoch, &riyadh_tm); // gmtime_r, not localtime_r: riyadh_epoch already IS the shifted instant, don't shift it twice

    instance.rtc.setDateTime(RTC_DateTime(
        riyadh_tm.tm_year + 1900,
        riyadh_tm.tm_mon + 1,
        riyadh_tm.tm_mday,
        riyadh_tm.tm_hour,
        riyadh_tm.tm_min,
        riyadh_tm.tm_sec));
    Serial.printf("RTC synced from NTP: %04d-%02d-%02d %02d:%02d:%02d (Asia/Riyadh)\n",
                  riyadh_tm.tm_year + 1900, riyadh_tm.tm_mon + 1, riyadh_tm.tm_mday,
                  riyadh_tm.tm_hour, riyadh_tm.tm_min, riyadh_tm.tm_sec);
    return true;
}

static void network_task(void *pvParameters)
{
    char wifi_ssid[64], wifi_pass[64];
    load_wifi_credentials(wifi_ssid, sizeof(wifi_ssid), wifi_pass, sizeof(wifi_pass));
    ensure_device_key();

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
    WiFi.mode(WIFI_STA);
    // Arduino-ESP32's built-in auto-reconnect retries roughly every 2s with
    // no backoff — fine normally, but bad credentials (typo'd during a
    // reprovision, say) turn that into a near-continuous retry loop, which
    // shares the radio with BLE closely enough to make BLE unreliable right
    // when it's needed most (to fix the bad credentials). Handling
    // reconnects manually below with real backoff avoids that.
    WiFi.setAutoReconnect(false);
    pick_boot_network(wifi_ssid, sizeof(wifi_ssid), wifi_pass, sizeof(wifi_pass));
    Serial.printf("Connecting to Wi-Fi network: %s\n", wifi_ssid);
    WiFi.begin(wifi_ssid, wifi_pass);
    unsigned long connect_start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - connect_start < 10000) {
        vTaskDelay(pdMS_TO_TICKS(300));
        Serial.print(".");
    }

    if (WiFi.status() != WL_CONNECTED && WIFI_AP_PROVISIONING_ENABLED) {
        Serial.println();
        Serial.println("Could not connect with saved/fallback credentials — starting Wi-Fi AP provisioning.");
        start_wifi_ap_provisioning();
        while (WiFi.status() != WL_CONNECTED) {
            portal_dns.processNextRequest();
            portal_server.handleClient();
            vTaskDelay(pdMS_TO_TICKS(10)); // short delay: handleClient() needs frequent pumping, unlike BLE's callback-driven model
            if (wifi_credentials_pending) break; // new credentials arrived via the portal — stop retrying the old ones
            if (cancel_wifi_setup_requested) {
                // X on the watch during this boot-time wait. This loop runs before
                // the main for(;;) that normally handles cancel, so without this
                // the AP kept broadcasting and its PIN never changed.
                cancel_wifi_setup_requested = false;
                ble_restart_requested = false;
                stop_wifi_ap_provisioning();
                portal_ready_for_ui = false;
                Serial.println("Wi-Fi setup canceled from the watch.");
                break;
            }
        }
        if (WiFi.status() == WL_CONNECTED) {
            stop_wifi_ap_provisioning(); // the original credentials worked after all, just slower than 10s
        }
    } else if (WiFi.status() != WL_CONNECTED && BLE_PROVISIONING_ENABLED) {
        Serial.println();
        Serial.println("Could not connect with saved/fallback credentials — starting BLE provisioning.");
        start_ble_provisioning();
        while (WiFi.status() != WL_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(300));
            Serial.print(".");
            if (wifi_credentials_pending) break; // new credentials arrived over BLE — stop retrying the old ones
            if (cancel_wifi_setup_requested) {
                cancel_wifi_setup_requested = false;
                ble_restart_requested = false;
                stop_ble_provisioning();
                portal_ready_for_ui = false;
                Serial.println("Wi-Fi setup canceled from the watch.");
                break;
            }
        }
        if (WiFi.status() == WL_CONNECTED) {
            stop_ble_provisioning(); // the original credentials worked after all, just slower than 10s
        }
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("Wi-Fi connected.");
        Serial.print("Watch IP address: ");
        Serial.println(WiFi.localIP());
        ensure_mdns_service();
        sync_rtc_from_ntp();
    } else {
        Serial.println("Wi-Fi not connected yet — BLE provisioning is disabled, will keep retrying in the background.");
    }

    server.begin();
    Serial.printf("TCP server listening on port %u\n", TCP_PORT);
    Serial.println("Send 'r'/'s' to start/stop mic streaming, or '#'+code (e.g. '#B') to simulate a result.");

    bool wifi_reconnect_pending = false; // true from the moment new BLE-provisioned credentials are applied until we know whether they worked
    bool wifi_scan_in_progress = false;  // true from starting an async scan (Wi-Fi settings screen) until scanComplete() stops returning WIFI_SCAN_RUNNING
    unsigned long wifi_scan_started_ms = 0;
    unsigned long wifi_reconnect_started = 0;

    // General-purpose reconnect with backoff, for any other disconnect
    // (router hiccup, out of range, etc.) — separate from wifi_reconnect_pending
    // above, which is specifically the one-shot 15s-then-notify attempt
    // right after new BLE-provisioned credentials.
    unsigned long last_reconnect_attempt = millis();
    unsigned long reconnect_backoff_ms = 2000;

    // Periodic re-sync on top of the at-connect ones above: keeps correcting
    // for the RTC's own ongoing drift on a watch that stays powered on and
    // connected for a long time, instead of only ever getting one correction
    // per boot/reconnect. 6h is frequent enough that even a "bad" RTC (a few
    // seconds/day of drift) never accumulates more than a fraction of a
    // second of visible error between syncs.
    static const unsigned long NTP_RESYNC_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL;
    unsigned long last_ntp_sync_millis = millis(); // initial connect above just did one, if it succeeded
    bool was_connected_last_loop = (WiFi.status() == WL_CONNECTED);

    unsigned long dbg_loop_start = millis();
    for (;;) {
        unsigned long dbg_now = millis();
        if (dbg_now - dbg_loop_start > 1000) {
            Serial.printf("DBG network loop stalled %lu ms\n", dbg_now - dbg_loop_start);
        }
        dbg_loop_start = dbg_now;

        // Skip entirely while BLE is actively open: that already means Wi-Fi
        // isn't working and someone may be mid-way through fixing it over
        // BLE right now — competing background reconnect attempts (even
        // backed off) are exactly what made BLE unreliable last time.
        if (portal_active) {
            portal_dns.processNextRequest();
            portal_server.handleClient();
        }

        if (!wifi_reconnect_pending && !ble_active && !portal_active && wifi_ssid[0] != '\0') {
            if (WiFi.status() == WL_CONNECTED) {
                reconnect_backoff_ms = 2000; // reset once healthy
            } else if (millis() - last_reconnect_attempt >= reconnect_backoff_ms) {
                last_reconnect_attempt = millis();
                Serial.printf("Wi-Fi disconnected — retrying (next backoff %lums)...\n", reconnect_backoff_ms);
                WiFi.begin(wifi_ssid, wifi_pass);
                reconnect_backoff_ms = (reconnect_backoff_ms * 2 > RECONNECT_BACKOFF_MAX_MS) ? RECONNECT_BACKOFF_MAX_MS : reconnect_backoff_ms * 2;
            }
        }

        // Catches the plain "reconnected after a router hiccup" case handled
        // just above (that path has no "just reconnected" branch of its own
        // like the reprovisioning ones do), plus the periodic 6h re-sync
        // declared before this loop — both share the same guard so a fresh
        // sync always follows any moment Wi-Fi goes from down to up.
        bool is_connected_now = (WiFi.status() == WL_CONNECTED);
        if (!is_connected_now && was_connected_last_loop && mdns_started) {
            MDNS.end();
            mdns_started = false;
            Serial.println("mDNS stopped because Wi-Fi disconnected.");
        }
        if (!is_connected_now && was_connected_last_loop && WIFI_AP_PROVISIONING_ENABLED && !portal_active && !ble_active) {
            Serial.println("Wi-Fi lost after a successful connection; starting setup network.");
            start_wifi_ap_provisioning();
        } else if (is_connected_now) {
            ensure_mdns_service();
        }
        if (is_connected_now && (!was_connected_last_loop || millis() - last_ntp_sync_millis >= NTP_RESYNC_INTERVAL_MS)) {
            if (sync_rtc_from_ntp()) {
                last_ntp_sync_millis = millis();
            }
        }
        was_connected_last_loop = is_connected_now;

        if (wifi_reconnect_pending) {
            if (WiFi.status() == WL_CONNECTED) {
                wifi_reconnect_pending = false;
                Serial.println("Reprovisioned Wi-Fi connected.");
                Serial.print("Watch IP address: ");
                Serial.println(WiFi.localIP());
                // نحفظ بس هنا — بعد ما نتأكد إنها اشتغلت فعليًا — مو لحظة
                // ما توصل بيانات الاعتماد. حفظها فورًا (السلوك القديم) كان
                // يخلي كلمة مرور غلط تدخل قائمة الشبكات المحفوظة، فتحاول
                // الساعة تتصل فيها تلقائيًا للأبد وتدخل وضع الإعداد من كل
                // إقلاع — بالضبط المشكلة اللي واجهناها مع HUAWEI-B535-447F.
                add_or_update_saved_network(wifi_ssid, wifi_pass);
                ensure_mdns_service();
                sync_rtc_from_ntp();
                last_ntp_sync_millis = millis();
                was_connected_last_loop = true; // already synced above — don't have the loop's own check redo it immediately
                if (status_characteristic) {
                    status_characteristic->setValue("K");
                    status_characteristic->notify();
                }
                stop_ble_provisioning(); // only stops advertising now — safe even with a client still connected, see its own comment
                wifi_switch_in_progress = false; // clears the guard set in connect_to_saved_network_cb — was never reset, so only the very first tap on a saved network ever worked
                wifi_saved_list_needs_refresh = true;
            } else if (millis() - wifi_reconnect_started > WIFI_RECONNECT_TIMEOUT_MS) {
                wifi_reconnect_pending = false;
                wifi_switch_in_progress = false; // same guard reset on the failure path
                Serial.printf("Reprovisioned Wi-Fi failed to connect within 15s — NOT saving '%s' (likely wrong password).\n", wifi_ssid);
                if (status_characteristic) {
                    status_characteristic->setValue("F");
                    status_characteristic->notify();
                }
                // نرجّع للشبكة المحفوظة الأخيرة اللي كانت تشتغل (إن وُجدت)
                // بدل ما نفضل نحاول بيانات فشلت للأبد بحلقة إعادة الاتصال
                // العامة تحت — تلك الحلقة تستخدم wifi_ssid/wifi_pass زي ما
                // هي، وما تعرف إنها فشلت لأول مرة الحين.
                if (saved_network_count > 0) {
                    strncpy(wifi_ssid, saved_networks[0].ssid, sizeof(wifi_ssid) - 1);
                    wifi_ssid[sizeof(wifi_ssid) - 1] = '\0';
                    strncpy(wifi_pass, saved_networks[0].pass, sizeof(wifi_pass) - 1);
                    wifi_pass[sizeof(wifi_pass) - 1] = '\0';
                } else {
                    wifi_ssid[0] = '\0';
                    wifi_pass[0] = '\0';
                }
                // Deliberately leave BLE running on failure so the user can
                // just try again with different credentials, instead of
                // having to reopen provisioning from Settings first.
            }
        }

        if (ble_restart_requested) {
            ble_restart_requested = false;
            if (WIFI_AP_PROVISIONING_ENABLED) {
                start_wifi_ap_provisioning();
            } else if (BLE_PROVISIONING_ENABLED) {
                start_ble_provisioning();
            } else {
                Serial.println("Change-Wi-Fi requested, but no provisioning method is enabled right now — ignoring.");
            }
        }

        static bool dbg_last_cancel_flag = false;
        if (cancel_wifi_setup_requested != dbg_last_cancel_flag) {
            Serial.printf("DBG cancel flag changed to %d (portal_active=%d)\n", cancel_wifi_setup_requested, portal_active);
            dbg_last_cancel_flag = cancel_wifi_setup_requested;
        }
        if (cancel_wifi_setup_requested) {
            cancel_wifi_setup_requested = false;
            ble_restart_requested = false; // in case "start" was tapped and "cancel" beat this loop to processing it
            if (portal_active) stop_wifi_ap_provisioning();
            if (ble_active) stop_ble_provisioning();
            portal_ready_for_ui = false; // covers canceling before the "ready" state ever showed
            Serial.println("Wi-Fi setup canceled from the watch.");
        }

        if (wifi_credentials_pending) {
            wifi_credentials_pending = false;
            strncpy(wifi_ssid, pending_wifi_ssid, sizeof(wifi_ssid) - 1);
            wifi_ssid[sizeof(wifi_ssid) - 1] = '\0';
            strncpy(wifi_pass, pending_wifi_pass, sizeof(wifi_pass) - 1);
            wifi_pass[sizeof(wifi_pass) - 1] = '\0';
            // ما نحفظها هنا — نستنى نتأكد إنها اتصلت فعليًا (انظر
            // wifi_reconnect_pending تحت) قبل ما تدخل قائمة الشبكات المحفوظة.
            Serial.printf("Reconnecting to newly provisioned Wi-Fi network: %s\n", wifi_ssid);
            if (portal_active) {
                stop_wifi_ap_provisioning(); // drops the AP and switches back to WIFI_STA before reconnecting
            }
            WiFi.disconnect();
            WiFi.begin(wifi_ssid, wifi_pass);
            wifi_reconnect_pending = true;
            wifi_reconnect_started = millis();
        }

        // "مسح كل الشبكات المحفوظة" من شاشة إعدادات الواي فاي — saved_networks[]
        // نفسها انمسحت فورًا من مهمة الواجهة (انظر تعليق wifi_reset_requested
        // عند تعريفها)، وهذا الجزء بس يقطع الراديو الفعلي ويوقف محاولات
        // إعادة الاتصال بالبيانات القديمة.
        if (wifi_reset_requested) {
            wifi_reset_requested = false;
            wifi_ssid[0] = '\0';
            wifi_pass[0] = '\0';
            WiFi.disconnect(true);
            Serial.println("All saved Wi-Fi networks cleared from the watch.");
        }

        // شاشة إعدادات الواي فاي تطلب مسح عشان تعرف أي شبكة محفوظة بالمدى
        // فعلاً (انظر تعليق wifi_scan_for_settings_requested عند تعريفها).
        // لو فيه مسح شغّال أصلاً (نادر، بس ممكن لو المستخدم فتح الشاشة
        // مرتين بسرعة) نتجاهل الطلب الجديد ونخلي اللي شغّال يكمل.
        //
        // لو الساعة بوضع الإعداد (portal_active — بثّ شبكتها الخاصة بدل ما
        // تكون عميل بشبكة حقيقية) ما نبدأ مسح جديد أصلًا: هذا الوضع أصلًا
        // يسوي مسحه الخاص لصفحة الإعداد (انظر start_wifi_ap_provisioning)،
        // وتشغيل مسحين بنفس الوقت كان يتعارض ويرجّع نتيجة فاضية بدون سبب
        // واضح للمستخدم. بدلها نرجّع "لا نتائج" فورًا.
        if (wifi_scan_for_settings_requested && !wifi_scan_in_progress) {
            wifi_scan_for_settings_requested = false;
            if (portal_active) {
                nearby_ssid_count = 0;
                wifi_scan_for_settings_ready = true;
            } else {
                WiFi.scanNetworks(true, false); // async؛ ما يهمنا نعرض شبكات مخفية، ما فيه اسم نعرضه لها أصلًا
                wifi_scan_in_progress = true;
                wifi_scan_started_ms = millis();
            }
        }
        if (wifi_scan_in_progress) {
            int n = WiFi.scanComplete();
            // مهلة أمان: لو المسح ما خلص خلال WIFI_SCAN_TIMEOUT_MS (تعارض مع
            // مسح ثاني، أو الراديو مشغول باتصال يعيد المحاولة) نوقف الانتظار
            // ونعتبره فشل بدل ما تعلّق شاشة "جارٍ البحث..." للأبد.
            bool timed_out = millis() - wifi_scan_started_ms > WIFI_SCAN_TIMEOUT_MS;
            if (n != WIFI_SCAN_RUNNING || timed_out) {
                if (n == WIFI_SCAN_FAILED || n == WIFI_SCAN_RUNNING) n = 0; // n لسا RUNNING فقط لو دخلنا هنا بسبب المهلة
                nearby_ssid_count = 0;
                for (int i = 0; i < n && nearby_ssid_count < MAX_SCAN_RESULTS; i++) {
                    String ssid = WiFi.SSID(i);
                    if (ssid.length() == 0) continue; // شبكة مخفية — ما نقدر نطابقها باسم أصلًا
                    nearby_ssids[nearby_ssid_count++] = ssid;
                }
                WiFi.scanDelete();
                wifi_scan_in_progress = false;
                wifi_scan_for_settings_ready = true;
            }
        }

        // Checked unconditionally, every loop iteration — not just when the
        // current client "looks" disconnected. client.connected() only goes
        // false once this side notices a clean FIN; a phone app killed or
        // backgrounded mid-connection (no clean close) leaves it reading
        // true indefinitely.
        //
        // So a new connection must be able to replace the current one — but
        // only once it has proven it knows device_key. Until then it waits in
        // pending_client and the current session keeps running untouched, so
        // an unauthenticated connection (a stranger on the network, or an app
        // build that doesn't speak the handshake) can never kick the real app
        // off or cut an audio stream short. The legit app reconnecting after
        // its own socket died authenticates and takes over immediately.
        if (security_reset_requested) {
            security_reset_requested = false;
            forget_pairing();
            if (client) client.stop();         // the old key was just revoked — neither connection can stay trusted
            if (pending_client) pending_client.stop();
        }

        WiFiClient newClient = server.available();
        if (newClient) {
            if (pending_client) {
                Serial.println("Security: newer connection replaced an unfinished handshake.");
                pending_client.stop();
            }
            pending_client = newClient;
            pending_client.setNoDelay(true);
            auth_line_len = 0;
            pending_is_pairing = !device_paired;
            Serial.println("Client connecting — awaiting authentication.");
            if (pending_is_pairing) {
                unsigned long t0 = millis();
                if (!pairing_begin(pair_watch_pub)) {
                    Serial.println("Security: X25519 key generation failed — dropping connection.");
                    pending_client.stop();
                } else {
                    char pub_hex[sizeof(pair_watch_pub) * 2 + 1];
                    bytes_to_hex(pair_watch_pub, sizeof(pair_watch_pub), pub_hex);
                    pending_client.printf("PAIR,%s\n", pub_hex);
                    pending_stage = PENDING_AWAIT_PAIR;
                    auth_challenge_sent_ms = millis();
                    Serial.printf("Security: pairing started (key generation took %lu ms).\n", millis() - t0);
                }
            } else {
                memcpy(pending_key, device_key, DEVICE_KEY_LEN);
                send_auth_challenge();
            }
        }

        if (pending_client) {
            bool authenticated = false;
            if (millis() - auth_challenge_sent_ms > AUTH_TIMEOUT_MS) {
                Serial.println("Security: client didn't authenticate in time — dropping connection.");
                pending_client.stop();
            } else {
                int drained = 0;
                while (pending_client && pending_client.available() && drained++ < 512) {
                    char c = (char)pending_client.read();
                    if (c == '\n' || c == '\r') {
                        if (auth_line_len == 0) continue;
                        auth_line_buf[auth_line_len] = '\0';
                        auth_line_len = 0;

                        if (pending_stage == PENDING_AWAIT_PAIR) {
                            // "PAIR,<64 hex chars>" = the phone's ephemeral X25519 public key
                            uint8_t phone_pub[32];
                            unsigned long t0 = millis();
                            if (strncmp(auth_line_buf, "PAIR,", 5) != 0 ||
                                !hex_to_bytes(auth_line_buf + 5, strlen(auth_line_buf + 5), phone_pub, sizeof(phone_pub))) {
                                Serial.println("Security: malformed PAIR response — dropping connection.");
                                pending_client.stop();
                                break;
                            }
                            if (!pairing_finish(pair_watch_pub, phone_pub, pending_key)) {
                                Serial.println("Security: invalid phone public key — dropping connection.");
                                pending_client.stop();
                                break;
                            }
                            Serial.printf("Security: pairing key agreed (%lu ms) — verifying it.\n", millis() - t0);
                            send_auth_challenge(); // proves both sides derived the same key before it's saved
                            continue;
                        }

                        // "AUTH,<64 hex chars>" = HMAC-SHA256(pending_key, auth_nonce), hex-encoded
                        uint8_t resp_hmac[32];
                        uint8_t expected[32];
                        if (strncmp(auth_line_buf, "AUTH,", 5) != 0 ||
                            !hex_to_bytes(auth_line_buf + 5, strlen(auth_line_buf + 5), resp_hmac, sizeof(resp_hmac))) {
                            Serial.println("Security: malformed AUTH response — dropping connection.");
                            pending_client.stop();
                        } else {
                            compute_hmac_sha256(pending_key, DEVICE_KEY_LEN, auth_nonce, sizeof(auth_nonce), expected);
                            if (memcmp(resp_hmac, expected, sizeof(expected)) == 0) {
                                authenticated = true;
                                if (pending_is_pairing) {
                                    save_paired_key(pending_key);
                                    Serial.println("Security: paired with a new phone.");
                                }
                            } else {
                                // Explicit reply before closing, so the app can tell "key rejected"
                                // (delete it, re-pair) apart from a Wi-Fi drop (keep it, retry).
                                pending_client.print("AUTH,FAIL\n");
                                Serial.println("Security: wrong device key — dropping connection.");
                                pending_client.stop();
                            }
                        }
                        break; // one response line per handshake — anything after it belongs to the session
                    } else if (auth_line_len < sizeof(auth_line_buf) - 1) {
                        auth_line_buf[auth_line_len++] = c;
                    } else {
                        Serial.println("Security: AUTH response too long — dropping connection.");
                        pending_client.stop();
                        auth_line_len = 0;
                        break;
                    }
                }
            }

            if (authenticated) {
                if (client && client.connected()) {
                    Serial.println("Authenticated client replacing previous connection.");
                    client.stop();
                }
                if (streaming) {
                    streaming = false;
                    stop_microphone(); // the previous session's stream shouldn't keep the mic open under the new one
                }
                client = pending_client;
                pending_client = WiFiClient();
                client.print("AUTH,OK\n");
                was_connected = true;
                last_client_seen_millis = millis();
                connection_start_millis = millis();
                result_parse_state = RESULT_IDLE; // a previous connection can't leave half-read state behind
                reminder_rx_active = false;       // ولا نصف جدول تذكيرات يبلع أول بايتات الاتصال الجديد
                Serial.println("Security: client authenticated.");
                Serial.println("Client connected.");
            }
        }

        if (!client || !client.connected()) {
            if (was_connected) {
                Serial.println("Client disconnected.");
                was_connected = false;
                streaming = false;
                stop_microphone();
                last_disconnect_millis = millis();
                ever_connected = true;
            }
        }

        // A genuinely open socket always counts as "seen" on its own — this
        // matters for mic streaming, where the watch is only ever *sending*
        // audio to the phone and client.available() (bytes coming *from* the
        // phone) can stay false for the whole session, which would otherwise
        // let a long streaming session time out even though it's clearly
        // still connected.
        if (client && client.connected()) {
            last_client_seen_millis = millis();
        }

        // نستنزف كل البايتات المتاحة بكل دورة، مو بايت واحد فقط. سببين،
        // وكلاهما ظهر فعليًا مع أمر جدول التذكيرات '@':
        //  ١) هذي الحلقة تنتهي بـ vTaskDelay(2ms)، فبايت/دورة يعني ~٢ ملي
        //     ثانية للبايت الواحد. أمر '#T11' طوله ٤ بايت فيمر بسهولة، لكن
        //     جدول التذكيرات عشرات البايتات — والجوال يقفل الاتصال بعد ما
        //     يرسل، فينقطع الجدول بالنص.
        //  ٢) الشرط القديم كان يشترط connected()، و WiFiClient يعتبر
        //     الاتصال منتهيًا لحظة وصول FIN حتى لو باقي بايتات مخزّنة عندنا
        //     — فبقية الرسالة كانت تُرمى بدل ما تُقرأ. البايتات المخزّنة
        //     صالحة تمامًا بعد الإغلاق، فنقرأها على أساس available() وحدها.
        // الحد ٥١٢ عشان دفعة كبيرة ما تجوّع إرسال الصوت بنفس الحلقة.
        if (client) {
            int drained = 0;
            while (client.available() && drained++ < 512) {
                handle_incoming_byte(client.read(), "client");
            }
        }

        // Real state lives here, not in the connect/disconnect branches above:
        // "connected" means either a socket is open right now, or one was
        // seen within the last CLIENT_ACTIVITY_TIMEOUT_MS — covering both a
        // future persistent connection (always "seen", never times out while
        // truly open) and today's connect-poll-disconnect pattern (bridges
        // the gap between polls).
        phone_connected = last_client_seen_millis != 0 &&
                          (millis() - last_client_seen_millis) < CLIENT_ACTIVITY_TIMEOUT_MS;

        // Mic reads block for ~32ms per chunk (fine). client.write() must not
        // block, though: when the phone vanishes mid-stream (Wi-Fi off), its
        // retry loop waits up to 10 x 1s for the socket, freezing this whole
        // task — seen as "network loop stalled 10025 ms" — so auth handshakes
        // (5s timeout), results and reconnects all go unanswered. So we only
        // write when select() says the socket can take data now (lwIP reports
        // writable only with about half its send buffer free, far more than
        // one 1KB chunk, so the write itself then doesn't wait). Otherwise the
        // chunk is dropped whole — stale live audio is worthless and dropping
        // whole chunks keeps the PCM16 sample alignment intact — and after
        // AUDIO_SEND_STALL_MS with nothing accepted the phone is treated as
        // gone. (availableForWrite() can't be used for this: NetworkClient
        // never overrides it, so it always returns 0.)
        if (streaming && client && client.connected()) {
            size_t n = instance.mic.readBytes((char *)audio_buf, CHUNK_SIZE);
            if (!audio_read_started) {
                audio_read_started = true;
                Serial.printf("Microphone read started: requested=%u bytes, received=%u bytes.\n",
                              (unsigned)CHUNK_SIZE, (unsigned)n);
            }
            if (n > 0) {
                apply_mic_gain(audio_buf, n, MIC_SOFTWARE_GAIN);
                size_t written = socket_writable_now(client.fd()) ? client.write(audio_buf, n) : 0;
                if (written == n) {
                    if (audio_drop_count > 0) {
                        Serial.printf("Streaming: phone caught up after %lu dropped chunks.\n", audio_drop_count);
                        audio_drop_count = 0;
                    }
                    last_audio_send_ok_ms = millis();
                } else if (written == 0) {
                    if (audio_drop_count++ == 0) Serial.println("Streaming: phone not receiving — dropping audio chunks.");
                    if (millis() - last_audio_send_ok_ms > AUDIO_SEND_STALL_MS) {
                        Serial.println("Streaming: phone accepted nothing for 3s — dropping the connection.");
                        client.stop();
                        streaming = false;
                        stop_microphone();
                        audio_drop_count = 0;
                        continue;
                    }
                    vTaskDelay(pdMS_TO_TICKS(2));
                    continue;
                }
                audio_chunks_sent++;
                audio_bytes_sent += written;
                if (audio_chunks_sent == 1 || audio_chunks_sent % 16 == 0) {
                    // مستوى ذروة العينات بعد التضخيم — مو مجرد عدد البايتات —
                    // عشان نقدر نشوف رقميًا فرق قوة الصوت بين قريب وبعيد أثناء
                    // الاختبار، بدل ما نخمّن من عدد البايتات (اللي ثابت دايمًا).
                    // ١٦ حزمة ≈ نص ثانية، يعطي دقة كافية لاختبار قريب/بعيد قصير.
                    const int16_t *samples = (const int16_t *)audio_buf;
                    size_t sample_count = n / sizeof(int16_t);
                    int16_t peak = 0;
                    for (size_t i = 0; i < sample_count; i++) {
                        int16_t v = samples[i];
                        if (v < 0) v = -v;
                        if (v > peak) peak = v;
                    }
                    Serial.printf("Audio packet %lu: mic=%u bytes, tcp_written=%u, total=%lu, peak=%d/32767 (%d%%).\n",
                                  audio_chunks_sent, (unsigned)n, (unsigned)written,
                                  audio_bytes_sent, peak, (int)(peak * 100L / 32767));
                }
                if (audio_chunks_sent == 1 && written == n) {
                    Serial.println("first audio packet sent");
                }
                if (written != n) {
                    Serial.printf("Audio short write: expected=%u, written=%u, connected=%s.\n",
                                  (unsigned)n, (unsigned)written,
                                  client.connected() ? "yes" : "no");
                }
            } else {
                Serial.println("Microphone returned 0 bytes while streaming.");
            }
        } else if (streaming && (!client || !client.connected())) {
            Serial.println("Streaming stopped: TCP client disconnected.");
            streaming = false;
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

// يفحص الجدول مرة كل دقيقة مقابل ساعة الساعة نفسها. يُنادى من loop() (مهمة
// الواجهة): handle_result_code ما يلمس الشاشة ولا محرك الاهتزاز بنفسه، بس
// يحط رسالة بالطابور اللي يفرّغه loop() — نفس المسار اللي يمشي فيه أي تنبيه
// جاي من الجوال، فما فيه أي تعامل خاص مع I2C هنا.
static void check_watch_reminders()
{
    if (watch_reminder_count == 0) return;

    // من اللقطة المشتركة، مو بقراءة I2C جديدة: هذي الدالة تُنادى من loop()
    // يعني مئات المرات بالثانية. انظر التعليق عند RtcSnapshot.
    const RtcSnapshot now = rtc_snapshot;
    if (!now.valid) return;
    // قبل أول مزامنة NTP ممكن يكون وقت الساعة غلط تمامًا (أو قيمة البذرة
    // الأولية) — ما نطلق تذكيرًا على وقت ما نثق فيه.
    if (now.year < 2025) return;

    static int last_minute_checked = -1;
    if (now.minute == last_minute_checked) return;
    last_minute_checked = now.minute;

    int today = weekday_iso(now.year, now.month, now.day); // ١..٧
    uint8_t today_bit = (uint8_t)(1 << (today - 1));

    // سطر واحد كل دقيقة وقت التشخيص: يخلي "ليش ما اشتغل التذكير؟" سؤالًا
    // له جواب مكتوب بدل تخمين. أطفئه بـ REMINDER_DEBUG_LOG بعد ما يستقر.
#if REMINDER_DEBUG_LOG
    Serial.printf("Reminders: الوقت %02d:%02d يوم=%d (bit=0x%02X) — %d مخزّنة\n",
                  now.hour, now.minute, today, today_bit, watch_reminder_count);
#endif

    for (int i = 0; i < watch_reminder_count; i++) {
        WatchReminder &r = watch_reminders[i];
        if (r.hour != now.hour || r.minute != now.minute) continue;
        // من هنا: الوقت مطابق. أي سبب يمنع الإطلاق يُطبع صراحة، ما يُبلع بصمت.
        if (r.spent) {
            Serial.printf("Reminders: [%d] وقته الآن لكنه تذكير مرة-وحدة اشتغل سابقًا — تخطّي\n", i);
            continue;
        }
        if (!(r.days_mask & today_bit)) {
            Serial.printf("Reminders: [%d] وقته الآن لكن اليوم مو ضمن أيامه (mask=0x%02X، اليوم=0x%02X) — تخطّي\n",
                          i, r.days_mask, today_bit);
            continue;
        }

        if (r.once) r.spent = true; // بالذاكرة فقط: الجوال يرسل جدولًا محدّثًا بدون هذا التذكير على أي حال
        handle_result_code('T', r.pattern, r.intensity, "watch-clock", r.label);
    }
}

void setup()
{
    Serial.setTxBufferSize(4096); // headroom so the zero-timeout setting below only drops logs when nobody's reading
    Serial.begin(115200);
    // With USB plugged into a computer that isn't reading the port (e.g. just
    // charging from a laptop), every Serial write otherwise blocks up to ~2s
    // (20 x 100ms retries in HWCDC) once its buffer fills — enough log volume
    // freezes network_task and the phone's 5s handshake times out. Logging
    // must never stall the device: drop output instead of waiting.
    Serial.setTxTimeoutMs(0);
    delay(300);

    instance.begin(); // also sets up instance.mic: PDM, 16kHz, mono, 16-bit
    beginLvglHelper(instance);

    build_splash_screen();
    build_onboarding_screen();
    build_home_screen();
    build_wifi_toast();
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
    load_reminders_from_prefs();
    update_clock_display_cb(NULL);
    // 1s, not 30s: the clock label only shows HH:MM, so a 30s period let the
    // face lag up to half a minute behind the real minute rollover. The
    // callback guards its own redraws, so most of these ticks do nothing.
    lv_timer_create(update_clock_display_cb, 1000, NULL);
    lv_timer_create(update_wifi_status_display_cb, 300, NULL);
    lv_timer_create(update_connection_status_poll_cb, 500, NULL);
    lv_timer_create(update_wifi_current_network_display_cb, 1000, NULL);
    lv_timer_create(update_wifi_scan_result_poll_cb, 300, NULL);
    update_connection_status(); // paint the correct (now real, not-connected-by-default) initial state immediately, don't wait for the first timer tick

    instance.setBrightness(DEVICE_MAX_BRIGHTNESS_LEVEL);

    result_queue = xQueueCreate(4, sizeof(ResultMessage));
    // Core 1 runs the Arduino loop()/LVGL; pin the network/audio task to the
    // other core (0) so it can never contend with UI rendering for CPU time.
    xTaskCreatePinnedToCore(network_task, "network", 16384, NULL, 1, NULL, 0);
}

void loop()
{
    lv_timer_handler();

    // BLE radio activity can occasionally corrupt whatever's mid-draw on the
    // display (see start_ble_provisioning()'s comment) — LVGL only redraws
    // regions that changed, so a corrupted patch otherwise sits there until
    // something forces a full redraw. Force one every second for as long as
    // BLE is actually advertising, so any corruption self-heals quickly
    // instead of requiring a reboot. Only runs during the short, user-
    // attended provisioning window, not normal operation.
    static unsigned long last_ble_redraw = 0;
    if (ble_active && millis() - last_ble_redraw > 1000) {
        last_ble_redraw = millis();
        lv_obj_invalidate(lv_screen_active());
    }

    ResultMessage msg;
    if (xQueueReceive(result_queue, &msg, 0) == pdTRUE) {
        show_result_category(msg);
        trigger_vibration(msg.pattern, msg.intensity); // I2C on the same bus as RTC/PMU — UI task only, see result_queue's comment
    }

    if (pending_vib_test_effect > 0) {
        int effect = pending_vib_test_effect;
        pending_vib_test_effect = 0;
        Serial.printf("Vib test: effect %d\n", effect);
        instance.setHapticEffects((uint8_t)effect);
        instance.vibrator();
    }

    check_watch_reminders();

    if (Serial.available()) {
        handle_incoming_byte(Serial.read(), "Serial");
    }

    delay(2);
}
