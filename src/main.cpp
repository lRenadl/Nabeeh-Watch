/**
 * Microphone bring-up test (temporary — replaces the main UI, see
 * main_ui_backup.cpp at the project root to restore it).
 *
 * Press the physical BOOT button (GPIO0) to record 5 seconds of audio from
 * the onboard PDM mic and save it as a .wav file on the FFat partition.
 *
 * Speaker playback on this unit is unverified (LilyGoLib's POWER_SPEAK power
 * rail is marked "// TODO:" in its own source and maps to a PMU channel
 * LilyGo's own hardware doc lists as "Unused" — likely not this board's real
 * speaker rail), so instead: send 'd' + Enter over the Serial Monitor to
 * dump the last recording as base64, which a companion script on the
 * computer decodes back into a real, playable .wav file.
 */
#include <LilyGoLib.h>
#include <FFat.h>

#define TEST_BUTTON_PIN 0 // BOOT button

static uint8_t *last_wav = NULL;
static size_t last_wav_size = 0;

static const char b64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void printBase64(const uint8_t *data, size_t len)
{
    size_t i = 0;
    char line[77]; // 76 base64 chars + terminator, printed in chunks
    int line_pos = 0;
    while (i < len) {
        int bytes_in_group = (len - i >= 3) ? 3 : (int)(len - i);
        uint32_t octet_a = data[i];
        uint32_t octet_b = bytes_in_group > 1 ? data[i + 1] : 0;
        uint32_t octet_c = bytes_in_group > 2 ? data[i + 2] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;
        i += bytes_in_group;

        line[line_pos++] = b64_chars[(triple >> 18) & 0x3F];
        line[line_pos++] = b64_chars[(triple >> 12) & 0x3F];
        line[line_pos++] = bytes_in_group > 1 ? b64_chars[(triple >> 6) & 0x3F] : '=';
        line[line_pos++] = bytes_in_group > 2 ? b64_chars[triple & 0x3F] : '=';

        if (line_pos >= 76) {
            line[line_pos] = '\0';
            Serial.println(line);
            line_pos = 0;
        }
    }
    if (line_pos > 0) {
        line[line_pos] = '\0';
        Serial.println(line);
    }
}

static void dumpLastRecording()
{
    if (!last_wav || last_wav_size == 0) {
        Serial.println("No recording yet — press BOOT first.");
        return;
    }
    Serial.println("---WAV-BEGIN---");
    printBase64(last_wav, last_wav_size);
    Serial.println("---WAV-END---");
}

static void recordAndSave()
{
    Serial.println("Recording 5 seconds...");
    size_t wav_size = 0;
    uint8_t *wav_buffer = instance.mic.recordWAV(5, &wav_size);
    if (!wav_buffer) {
        Serial.println("Recording failed (mic read error or out of PSRAM)");
        return;
    }
    Serial.printf("Recorded %u bytes\n", (unsigned)wav_size);

    char path[32];
    int idx = 1;
    do {
        snprintf(path, sizeof(path), "/test_%d.wav", idx++);
    } while (FFat.exists(path));

    File f = FFat.open(path, FILE_WRITE);
    if (f) {
        size_t written = f.write(wav_buffer, wav_size);
        f.close();
        Serial.printf("Saved %s (%u/%u bytes)\n", path, (unsigned)written, (unsigned)wav_size);
    } else {
        Serial.println("Failed to open file for writing");
    }

    Serial.println("Trying speaker playback too (may be silent on this unit)...");
    instance.player.playWAV(wav_buffer, wav_size);

    if (last_wav) free(last_wav);
    last_wav = wav_buffer;
    last_wav_size = wav_size;

    Serial.println("Done. Press BOOT to record again, or send 'd' + Enter to dump this recording as base64.");
}

void setup()
{
    Serial.begin(115200);
    delay(300);

    pinMode(TEST_BUTTON_PIN, INPUT_PULLUP);

    instance.begin(); // also sets up instance.mic (PDM, 16kHz mono 16-bit) and instance.player

    if (!FFat.begin(true)) {
        Serial.println("FFat mount failed");
    }

    instance.powerControl(POWER_SPEAK, true); // speaker amp is off by default

    Serial.println("Mic test ready. Press BOOT to record 5s.");
}

void loop()
{
    static int last_state = HIGH;
    int state = digitalRead(TEST_BUTTON_PIN);

    if (last_state == HIGH && state == LOW) {
        delay(30); // debounce
        if (digitalRead(TEST_BUTTON_PIN) == LOW) {
            recordAndSave();
        }
    }
    last_state = state;

    if (Serial.available()) {
        char c = Serial.read();
        if (c == 'd' || c == 'D') {
            dumpLastRecording();
        }
    }

    delay(5);
}
