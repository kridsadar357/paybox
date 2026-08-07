#include <Arduino.h>
#include <lvgl.h>
#include "display.h" // Custom UI functions would go here if refactored
#include "esp_bsp.h" // For hardware initialization
#include "lv_port.h" // For LVGL porting
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <PNGdec.h>
#include <driver/i2s.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>

#define FIRMWARE_VERSION "1.0.0"

// FreeRTOS
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

extern "C"
{
    LV_FONT_DECLARE(sarabun_20);
    LV_FONT_DECLARE(sarabun_28);
}

// I2S audio output pin — ยืนยันแล้วว่าถูกต้องสำหรับบอร์ดนี้ (เช็คผ่าน SD_MMC pin ในไฟล์ pin config
// ชุดเดียวกันแล้วเจอการ์ดจริง แปลว่า pin ชุดนี้ตรงกับบอร์ดจริง รวมถึง audio pin ด้วย)
#define AUDIO_I2S_BCK_IO 42
#define AUDIO_I2S_LRCK_IO 2
#define AUDIO_I2S_DO_IO 41
// =================================
//   GLOBAL OBJECTS & HANDLES
// =================================
Preferences preferences;
static WebServer server(80);
static HTTPClient http;

// LVGL Screen & Widget Objects
static int payment_amount = 0;
static int time_left = 120;
static String input_amount_str = "0";

static lv_obj_t *screen_setup_ap;
static lv_obj_t *screen_connecting_wifi;
static lv_obj_t *screen_payment;
static lv_obj_t *qr_code_obj;
static lv_obj_t *confirm_btn;
static lv_obj_t *payment_amount_label;
static lv_obj_t *payment_status_label;
static lv_obj_t *numpad;
static lv_obj_t *thank_you_popup;
static lv_timer_t *qr_countdown_timer = NULL;
static TaskHandle_t payment_check_task_handle = NULL;
static lv_obj_t *countdown_label_global = NULL;
static lv_obj_t *numpad_overlay = NULL;
static lv_style_t style_numpad_bg;
static lv_style_t style_numpad_btn;
// FreeRTOS Handles
static QueueHandle_t network_queue;

// Idle Banner Slideshow (แสดงเต็มจอสลับรูปเมื่อไม่มีการแตะหน้าจอ)
#define MAX_BANNERS 5
static PNG banner_png; // ใช้ตัวเดียวร่วมกัน decode ทีละรูปตามลำดับ (ไม่ทำพร้อมกัน กันข้อมูลชนกัน)
static uint16_t *banner_img_bufs[MAX_BANNERS] = {NULL, NULL, NULL, NULL, NULL};
static int banner_img_ws[MAX_BANNERS] = {0};
static int banner_img_hs[MAX_BANNERS] = {0};
static volatile bool banner_slot_ready[MAX_BANNERS] = {false};
static lv_img_dsc_t banner_img_dscs[MAX_BANNERS];
static bool banner_active = false;
static uint32_t banner_idle_ms = 20000;
static int banner_slide_index = -1;
static lv_obj_t *banner_img_obj = NULL;
static lv_timer_t *banner_slide_timer = NULL;
// ใช้เป็นปลายทางชั่วคราวระหว่าง decode รูปแต่ละ slot (banner_png_draw_cb เขียนลงตรงนี้)
static uint16_t *banner_decode_target_buf = NULL;
static int banner_decode_target_w = 0;
static int banner_decode_target_h = 0;

// =================================
//   CONFIGURATION & CONSTANTS
// =================================
// Preferences Keys
const char *P_CONFIGURED = "configured";
const char *P_WIFI_SSID = "wifi_ssid";
const char *P_WIFI_PASS = "wifi_pass";
const char *P_OP_MODE = "op_mode";
// Mode 1: Pulse
const char *P_PULSE_PIN = "pulse_pin";
// Mode 2: Thank You
const char *P_THANKYOU_API = "ty_api";
const char *P_THANKYOU_MSG = "ty_msg";
// Mode 3: Payment
const char *P_PAY_INCREMENT = "pay_inc";
const char *P_PAY_GEN_QR = "pay_gen_qr";
const char *P_PAY_CHECK_STATUS = "pay_chk_stat";
const char *P_PAY_THANKYOU_MSG = "pay_ty_msg";
// เสียงแจ้งเตือนตอนชำระเงินสำเร็จ (ใช้ร่วมกันทุกโหมด)
const char *P_TTS_URL = "tts_url";
// OTA firmware update (ใช้ร่วมกันทุกโหมด)
const char *P_OTA_URL = "ota_url";
// Idle Banner Slideshow (ใช้ร่วมกันทุกโหมด)
const char *P_BANNER_URL[MAX_BANNERS] = {"banner_url_1", "banner_url_2", "banner_url_3", "banner_url_4", "banner_url_5"};
const char *P_BANNER_IDLE_SEC = "banner_idle";

// App State
enum AppState
{
    APP_STATE_INIT,
    APP_STATE_SETUP,
    APP_STATE_CONNECTING,
    APP_STATE_RUNNING
};
static AppState current_app_state = APP_STATE_INIT;
struct NetworkRequest
{
    char url[256];
    int amount;
};

void main_app_task(void *pvParameters);
void network_task(void *pvParameters);
void create_main_payment_screen();
void create_banner_screen();
void banner_fetch_task(void *pvParameters);
void play_payment_audio_task(void *pvParameters);
void ota_check_task(void *pvParameters);
static void idle_check_timer_cb(lv_timer_t *timer);
static void banner_dismiss_event_cb(lv_event_t *e);
void handle_web_save();
void show_loading_spinner();
static void minus_btn_event_cb(lv_event_t *e);
static void plus_btn_event_cb(lv_event_t *e);
static void confirm_event_cb(lv_event_t *e);
static void qr_countdown_timer_cb(lv_timer_t *timer);
void check_payment_status_task(void *pvParameters);
static void cancel_payment_event_cb(lv_event_t *e);
static void keypad_event_cb(lv_event_t *e);
static void amount_panel_event_cb(lv_event_t *e);
static void clear_amount_event_cb(lv_event_t *e);
static void animate_numpad(lv_obj_t *obj, int32_t v, bool show);
static void numpad_bottom_btn_event_cb(lv_event_t *e);

#include "web_server_html.h"

// Task to run the web server without blocking
void web_server_task(void *pvParameters)
{
    server.on("/", HTTP_GET, []()
              { server.send(200, "text/html", INDEX_HTML); });

    server.on("/scan", HTTP_GET, []()
              {
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; ++i) {
      if (i) json += ",";
      json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
    }
    json += "]";
    server.send(200, "application/json", json); });

    server.on("/save", HTTP_POST, handle_web_save);
    server.begin();
    Serial.println("Web Server Started");

    while (1)
    {
        server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void handle_web_save()
{
    Serial.println("Saving configuration...");
    preferences.begin("paybox-cfg", false);

    // Save WiFi
    preferences.putString(P_WIFI_SSID, server.arg("wifi_ssid"));
    preferences.putString(P_WIFI_PASS, server.arg("wifi_pass"));

    // Save Mode
    String modeStr = server.arg("op_mode");
    int mode = 0;
    if (modeStr == "pulse")
        mode = 1;
    if (modeStr == "thankyou")
        mode = 2;
    if (modeStr == "payment")
        mode = 3;
    preferences.putInt(P_OP_MODE, mode);

    // Save mode-specific settings
    switch (mode)
    {
    case 1:
        preferences.putInt(P_PULSE_PIN, server.arg("pulse_pin").toInt());
        break;
    case 2:
        preferences.putString(P_THANKYOU_API, server.arg("ty_api"));
        preferences.putString(P_THANKYOU_MSG, server.arg("ty_msg"));
        break;
    case 3:
        preferences.putInt(P_PAY_INCREMENT, server.arg("pay_inc").toInt());
        preferences.putString(P_PAY_GEN_QR, server.arg("pay_gen_qr"));
        preferences.putString(P_PAY_CHECK_STATUS, server.arg("pay_chk_stat"));
        preferences.putString(P_PAY_THANKYOU_MSG, server.arg("pay_ty_msg"));
        break;
    }

    // Save OTA Firmware Update (ใช้ร่วมกันทุกโหมด, ไม่บังคับ)
    preferences.putString(P_OTA_URL, server.arg("ota_url"));

    // Save Payment Voice Announcement (ใช้ร่วมกันทุกโหมด, ไม่บังคับ)
    preferences.putString(P_TTS_URL, server.arg("tts_url"));

    // Save Idle Banner Slideshow (ใช้ร่วมกันทุกโหมด, ไม่บังคับ, สูงสุด 5 รูป)
    for (int i = 0; i < MAX_BANNERS; i++)
    {
        String fieldName = "banner_url_" + String(i + 1);
        preferences.putString(P_BANNER_URL[i], server.arg(fieldName));
    }
    int bannerIdleSec = server.arg("banner_idle_sec").toInt();
    preferences.putInt(P_BANNER_IDLE_SEC, bannerIdleSec > 0 ? bannerIdleSec : 20);

    preferences.putBool(P_CONFIGURED, true);
    preferences.end();

    String response = "<h1>Settings Saved!</h1><p>Device will reboot in 3 seconds.</p>";
    server.send(200, "text/html", response);
    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP.restart();
}

void reset_countdown()
{
    time_left = 120;
}

void animate_numpad(lv_obj_t *obj, int32_t v_start, int32_t v_end, bool show)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, v_start, v_end);
    lv_anim_set_time(&a, 350); // ระยะเวลา Animation
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out); // Effect การเคลื่อนที่

    if (!show)
    { // ถ้าเป็นการซ่อน
        // ตั้งค่า callback ให้ซ่อน object จริงๆ หลังจาก animation จบ
        lv_anim_set_ready_cb(&a, [](lv_anim_t *anim)
                             { lv_obj_add_flag((lv_obj_t *)anim->var, LV_OBJ_FLAG_HIDDEN); });
    }
    else
    { // ถ้าเป็นการแสดง
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }

    lv_anim_start(&a);
}

static void numpad_bottom_btn_event_cb(lv_event_t *e)
{
    const char* action = (const char*)lv_event_get_user_data(e);

    if (strcmp(action, "confirm") == 0)
    {
        // ทำงานเหมือน confirm_event_cb เดิม
        payment_amount = input_amount_str.toFloat() * 1;
        if (payment_amount > 0)
        {
            show_loading_spinner();
            NetworkRequest req;
            req.amount = payment_amount;
            xQueueSend(network_queue, &req, portMAX_DELAY);
        }
    }
    if (numpad_overlay) {
        lv_obj_add_flag(numpad_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void keypad_event_cb(lv_event_t *e)
{
    lv_obj_t *keypad = lv_event_get_target(e);
    uint32_t btn_id = lv_btnmatrix_get_selected_btn(keypad);
    const char *txt = lv_btnmatrix_get_btn_text(keypad, btn_id);

    // ถ้ากดปุ่ม "ยกเลิก"
    if (strcmp(txt, "ยกเลิก") == 0)
    {
        animate_numpad(numpad, lv_obj_get_y(numpad), lv_disp_get_ver_res(NULL), false);
        return;
    }

    // ถ้ากดปุ่ม "ยืนยัน"
    if (strcmp(txt, "ยืนยัน") == 0)
    {
        show_loading_spinner();
        payment_amount = input_amount_str.toFloat() * 1;
        if (payment_amount > 0)
        {
            NetworkRequest req;
            req.amount = payment_amount;
            xQueueSend(network_queue, &req, portMAX_DELAY);
        }
        animate_numpad(numpad, lv_obj_get_y(numpad), lv_disp_get_ver_res(NULL), false);
        return;
    }

    // จัดการปุ่มตัวเลข, จุด, และ Backspace
    if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0)
    {
        if (input_amount_str.length() > 1)
        {
            input_amount_str.remove(input_amount_str.length() - 1);
        }
        else
        {
            input_amount_str = "0";
        }
    }
    else if (strcmp(txt, "00") == 0)
    {
        // เติมสองศูนย์รวดเดียว - ยอดหลักร้อย/หลักพันกดเร็วขึ้นมาก
        // ไม่ทำอะไรถ้ายอดยังเป็น 0 อยู่ กัน "000"
        if (input_amount_str != "0" && input_amount_str.length() < 8)
        {
            input_amount_str += "00";
        }
    }
    else
    {
        if (input_amount_str == "0")
        {
            input_amount_str = txt;
        }
        else if (input_amount_str.length() < 9)
        { // จำกัดความยาว
            input_amount_str += txt;
        }
    }

    // อัปเดต Label ที่แสดงผล
    lv_label_set_text(payment_amount_label, input_amount_str.c_str());
}

// ปุ่ม "ยกเลิก" บนหน้าจอหลัก - ล้างยอดกลับเป็น 0 (ไม่ใช่ปิด numpad แบบดีไซน์เดิม
// เพราะแป้นตัวเลขแสดงอยู่ตลอดเวลาแล้ว)
static void clear_amount_event_cb(lv_event_t *e)
{
    input_amount_str = "0";
    payment_amount = 0;
    if (payment_amount_label && lv_obj_is_valid(payment_amount_label))
    {
        lv_label_set_text(payment_amount_label, input_amount_str.c_str());
    }
}

static void amount_panel_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED)
    {
        if (numpad_overlay && lv_obj_has_flag(numpad_overlay, LV_OBJ_FLAG_HIDDEN))
        {
            lv_obj_clear_flag(numpad_overlay, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void create_ui_setup_ap_screen()
{
    if (lvgl_port_lock(0))
    {
        lv_obj_t *screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x003a57), LV_PART_MAIN);

        lv_obj_t *lbl_title = lv_label_create(screen);
        lv_label_set_text(lbl_title, "357Paybox Setup");
        lv_obj_set_style_text_color(lbl_title, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_24, LV_PART_MAIN);
        lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 20);

        lv_obj_t *lbl_info = lv_label_create(screen);
        lv_label_set_text(lbl_info, "1. Connect to '357Paybox' WiFi\n2. Open a browser to 192.168.5.1");
        lv_obj_set_style_text_color(lbl_info, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_align(lbl_info, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_center(lbl_info);

        lv_scr_load(screen);
        lvgl_port_unlock();
    }
}

// UI for WiFi Connecting Screen
void create_ui_connecting_wifi_screen(const char *ssid)
{
    if (lvgl_port_lock(0))
    {
        lv_obj_t *screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);

        lv_obj_t *spinner = lv_spinner_create(screen, 1000, 60);
        lv_obj_set_size(spinner, 80, 80);
        lv_obj_center(spinner);

        lv_obj_t *lbl_status = lv_label_create(screen);
        lv_label_set_text_fmt(lbl_status, "Connecting to\n%s...", ssid);
        lv_obj_set_style_text_color(lbl_status, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_align(lbl_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align_to(lbl_status, spinner, LV_ALIGN_OUT_BOTTOM_MID, 0, 15);

        lv_scr_load(screen);
        lvgl_port_unlock();
    }
}

// =================================================================
//   DARK PREMIUM THEME
// =================================================================
// จานสีกลางของทั้งแอป - แก้ที่เดียวเปลี่ยนได้ทุกหน้าจอ
#define COL_BG          lv_color_hex(0x12161C) // พื้นหลังหลัก
#define COL_SURFACE     lv_color_hex(0x1B2129) // การ์ด / พาเนล
#define COL_SURFACE_HI  lv_color_hex(0x232C36) // ปุ่มตัวเลข
#define COL_BORDER      lv_color_hex(0x2A323C) // เส้นขอบบาง ๆ
#define COL_TEXT        lv_color_hex(0xE8EDF2) // ตัวอักษรหลัก
#define COL_MUTED       lv_color_hex(0x8A97A6) // ตัวอักษรรอง
#define COL_ACCENT      lv_color_hex(0x14B8A6) // เขียวมิ้นท์ - ปุ่มยืนยัน
#define COL_ACCENT_DIM  lv_color_hex(0x0E7C72) // เขียวมิ้นท์ตอนกด
#define COL_DANGER      lv_color_hex(0xEF4444) // แดง - ยกเลิก / หมดเวลา

void style_init(void)
{
    // พื้นหลังของแป้นตัวเลข - โปร่งใส ให้กลืนกับพื้นหลังหน้าจอ
    lv_style_init(&style_numpad_bg);
    lv_style_set_bg_opa(&style_numpad_bg, LV_OPA_TRANSP);
    lv_style_set_border_width(&style_numpad_bg, 0);
    lv_style_set_pad_all(&style_numpad_bg, 0);
    lv_style_set_pad_gap(&style_numpad_bg, 8);

    // ปุ่มตัวเลขแต่ละปุ่ม
    lv_style_init(&style_numpad_btn);
    lv_style_set_bg_color(&style_numpad_btn, COL_SURFACE_HI);
    lv_style_set_bg_opa(&style_numpad_btn, LV_OPA_COVER);
    lv_style_set_text_color(&style_numpad_btn, COL_TEXT);
    lv_style_set_border_width(&style_numpad_btn, 1);
    lv_style_set_border_color(&style_numpad_btn, COL_BORDER);
    lv_style_set_radius(&style_numpad_btn, 10);
    lv_style_set_text_font(&style_numpad_btn, &lv_font_montserrat_26);
}

// =================================================================
//   IDLE BANNER (แสดงรูปเต็มจอเมื่อไม่มีการแตะหน้าจอ)
// =================================================================

// เรียกทีละบรรทัด (scanline) โดย PNGdec ระหว่าง decode — แปลงเป็น RGB565 ใส่ลง buffer ปลายทาง
// (banner_decode_target_* ถูกตั้งไว้ก่อนเรียก decode() ของแต่ละ slot ตามลำดับ ไม่ทำพร้อมกันหลาย slot)
static void banner_png_draw_cb(PNGDRAW *pDraw)
{
    if (banner_decode_target_buf != NULL && pDraw->y < banner_decode_target_h)
    {
        // 0x00FFFFFF = blend พื้นที่โปร่งใสของรูปกับพื้นขาว (banner screen พื้นขาว)
        banner_png.getLineAsRGB565(pDraw, &banner_decode_target_buf[pDraw->y * banner_decode_target_w], PNG_RGB565_BIG_ENDIAN, 0x00FFFFFF);
    }
}

// ถอดรหัส PNG ที่โหลดมาไว้ใน RAM แล้วเก็บผลเป็น RGB565 ไว้ใช้ซ้ำที่ slot ที่ระบุ
static void decode_and_cache_banner_png(uint8_t *png_data, int png_size, int slot)
{
    int rc = banner_png.openRAM(png_data, png_size, banner_png_draw_cb);
    if (rc != PNG_SUCCESS)
    {
        Serial.printf("Banner[%d]: openRAM failed, rc=%d\n", slot, rc);
        return;
    }

    int w = banner_png.getWidth();
    int h = banner_png.getHeight();
    // จำกัดขนาดต่อรูปให้เล็กลงกว่าตอนมีรูปเดียว เพราะตอนนี้อาจมีสูงสุด 5 รูปพร้อมกันใน PSRAM
    if (w <= 0 || h <= 0 || (uint32_t)w * (uint32_t)h * 2 > 900 * 1024)
    {
        Serial.printf("Banner[%d]: invalid or too-large image %dx%d\n", slot, w, h);
        banner_png.close();
        return;
    }

    uint16_t *buf = (uint16_t *)heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM);
    if (buf == NULL)
    {
        Serial.printf("Banner[%d]: out of PSRAM for image buffer\n", slot);
        banner_png.close();
        return;
    }

    banner_decode_target_buf = buf;
    banner_decode_target_w = w;
    banner_decode_target_h = h;

    rc = banner_png.decode(NULL, 0);
    banner_png.close();
    banner_decode_target_buf = NULL;

    if (rc != PNG_SUCCESS)
    {
        Serial.printf("Banner[%d]: decode failed, rc=%d\n", slot, rc);
        heap_caps_free(buf);
        return;
    }

    banner_img_bufs[slot] = buf;
    banner_img_ws[slot] = w;
    banner_img_hs[slot] = h;
    banner_slot_ready[slot] = true;
    Serial.printf("Banner[%d]: image ready %dx%d\n", slot, w, h);
}

// โหลดรูปทีละ URL ตามลำดับ (ไม่พร้อมกัน กัน PNG decoderตัวเดียวชนกัน) มาไว้ใน RAM ครั้งเดียวตอนบูต
// แล้วถอดรหัสเก็บไว้ ไม่ต้องโหลดซ้ำทุกครั้งที่ idle
void banner_fetch_task(void *pvParameters)
{
    String *urls = (String *)pvParameters;

    for (int slot = 0; slot < MAX_BANNERS; slot++)
    {
        if (urls[slot].length() == 0)
        {
            continue;
        }

        HTTPClient banner_http;
        if (banner_http.begin(urls[slot]))
        {
            int httpCode = banner_http.GET();
            if (httpCode == HTTP_CODE_OK)
            {
                int len = banner_http.getSize();
                if (len > 0 && len < 900 * 1024)
                {
                    uint8_t *png_buf = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
                    if (png_buf != NULL)
                    {
                        WiFiClient *stream = banner_http.getStreamPtr();
                        int total_read = 0;
                        unsigned long start_time = millis();
                        while (total_read < len && banner_http.connected() && (millis() - start_time) < 20000)
                        {
                            int avail = stream->available();
                            if (avail > 0)
                            {
                                int to_read = avail < (len - total_read) ? avail : (len - total_read);
                                int r = stream->readBytes(png_buf + total_read, to_read);
                                total_read += r;
                            }
                            else
                            {
                                vTaskDelay(pdMS_TO_TICKS(5));
                            }
                        }

                        if (total_read == len)
                        {
                            decode_and_cache_banner_png(png_buf, len, slot);
                        }
                        else
                        {
                            Serial.printf("Banner[%d]: download incomplete (%d/%d bytes)\n", slot, total_read, len);
                        }
                        heap_caps_free(png_buf);
                    }
                    else
                    {
                        Serial.printf("Banner[%d]: out of PSRAM for download buffer\n", slot);
                    }
                }
                else
                {
                    Serial.printf("Banner[%d]: invalid content length %d\n", slot, len);
                }
            }
            else
            {
                Serial.printf("Banner[%d]: HTTP GET failed, code=%d\n", slot, httpCode);
            }
            banner_http.end();
        }
    }

    delete[] urls;
    vTaskDelete(NULL);
}

// แสดงรูปที่ slot ที่ระบุบน banner_img_obj ที่มีอยู่แล้ว (ใช้ตอนเปิด banner ครั้งแรก และตอนสไลด์เปลี่ยนรูป)
static void banner_show_slide(int slot)
{
    if (banner_img_obj == NULL || !banner_slot_ready[slot])
    {
        return;
    }

    banner_img_dscs[slot].header.always_zero = 0;
    banner_img_dscs[slot].header.w = banner_img_ws[slot];
    banner_img_dscs[slot].header.h = banner_img_hs[slot];
    banner_img_dscs[slot].header.cf = LV_IMG_CF_TRUE_COLOR;
    banner_img_dscs[slot].data_size = (uint32_t)banner_img_ws[slot] * (uint32_t)banner_img_hs[slot] * 2;
    banner_img_dscs[slot].data = (const uint8_t *)banner_img_bufs[slot];

    lv_img_set_src(banner_img_obj, &banner_img_dscs[slot]);

    // ปรับขนาดรูปให้พอดีจอ (480x320) ตามสัดส่วนเดิม ไม่ว่าไฟล์ต้นทางจะขนาดเท่าไหร่ก็ตาม
    // (256 = ขนาดจริง 1:1 ใน lv_img_set_zoom, ไม่ขยายเกิน 256 กันภาพแตก)
    int zoom_w = (480 * 256) / banner_img_ws[slot];
    int zoom_h = (320 * 256) / banner_img_hs[slot];
    int zoom = zoom_w < zoom_h ? zoom_w : zoom_h;
    if (zoom > 256)
    {
        zoom = 256;
    }
    lv_img_set_zoom(banner_img_obj, zoom);
    lv_obj_center(banner_img_obj);
}

static void banner_slide_timer_cb(lv_timer_t *timer)
{
    for (int i = 1; i <= MAX_BANNERS; i++)
    {
        int next = (banner_slide_index + i) % MAX_BANNERS;
        if (banner_slot_ready[next])
        {
            banner_slide_index = next;
            banner_show_slide(next);
            break;
        }
    }
}

static void banner_dismiss_event_cb(lv_event_t *e)
{
    if (banner_slide_timer != NULL)
    {
        lv_timer_del(banner_slide_timer);
        banner_slide_timer = NULL;
    }
    banner_img_obj = NULL;
    lv_disp_trig_activity(NULL);
    create_main_payment_screen();
}

void create_banner_screen()
{
    int first_ready = -1;
    for (int i = 0; i < MAX_BANNERS; i++)
    {
        if (banner_slot_ready[i])
        {
            first_ready = i;
            break;
        }
    }
    if (first_ready < 0)
    {
        return;
    }

    lv_obj_t *old_screen = lv_scr_act();
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(screen, banner_dismiss_event_cb, LV_EVENT_CLICKED, NULL);

    banner_img_obj = lv_img_create(screen);
    banner_slide_index = first_ready;
    banner_show_slide(first_ready);

    if (banner_slide_timer != NULL)
    {
        lv_timer_del(banner_slide_timer);
    }
    banner_slide_timer = lv_timer_create(banner_slide_timer_cb, 5000, NULL); // เปลี่ยนรูปทุก 5 วิ

    lv_scr_load(screen);
    if (old_screen != NULL)
    {
        lv_obj_del_async(old_screen);
    }
}

static bool any_banner_ready()
{
    for (int i = 0; i < MAX_BANNERS; i++)
    {
        if (banner_slot_ready[i])
        {
            return true;
        }
    }
    return false;
}

static void idle_check_timer_cb(lv_timer_t *timer)
{
    if (!banner_active && any_banner_ready() && banner_idle_ms > 0 &&
        lv_disp_get_inactive_time(NULL) >= banner_idle_ms)
    {
        banner_active = true;
        create_banner_screen();
    }
}

void create_main_payment_screen()
{
    banner_active = false;
    input_amount_str = "0";
    payment_amount = 0;
    // screen เก่าจะถูกลบทิ้งด้านล่าง - ต้องล้าง pointer เก่าก่อน ไม่งั้นจะชี้ไปยัง object ที่ถูกลบแล้ว
    numpad_overlay = NULL;
    numpad = NULL;
    countdown_label_global = NULL;

    // สร้าง screen ใหม่ + lv_scr_load() แบบเดียวกับหน้าจออื่นๆ ในแอป (connecting/QR/setup)
    // แทนที่จะแก้ไข lv_scr_act() ในที่ — พบว่าการแก้ไข screen เดิมทำให้บาง object ไม่ถูกรีเฟรชขึ้นจอจริง
    lv_obj_t *old_screen = lv_scr_act();
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, COL_BG, 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0x0C1014), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    // ---------- แถบหัวจอ ----------
    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), 34);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *brand_dot = lv_obj_create(header);
    lv_obj_remove_style_all(brand_dot);
    lv_obj_set_size(brand_dot, 8, 8);
    lv_obj_align(brand_dot, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_radius(brand_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(brand_dot, COL_ACCENT, 0);
    lv_obj_set_style_bg_opa(brand_dot, LV_OPA_COVER, 0);

    lv_obj_t *brand = lv_label_create(header);
    lv_label_set_text(brand, "357 PAYBOX");
    lv_obj_set_style_text_font(brand, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(brand, COL_MUTED, 0);
    lv_obj_set_style_text_letter_space(brand, 2, 0);
    lv_obj_align(brand, LV_ALIGN_LEFT_MID, 32, 0);

    lv_obj_t *wifi_icon = lv_label_create(header);
    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(wifi_icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(wifi_icon,
                                WiFi.status() == WL_CONNECTED ? COL_ACCENT : COL_MUTED, 0);
    lv_obj_align(wifi_icon, LV_ALIGN_RIGHT_MID, -16, 0);

    // ---------- ฝั่งซ้าย: ยอดเงิน + ปุ่มสั่งงาน ----------
    lv_obj_t *left = lv_obj_create(screen);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, 236, 268);
    lv_obj_align(left, LV_ALIGN_TOP_LEFT, 16, 40);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *caption = lv_label_create(left);
    lv_label_set_text(caption, "ยอดชำระ");
    lv_obj_set_style_text_font(caption, &sarabun_20, 0);
    lv_obj_set_style_text_color(caption, COL_MUTED, 0);
    lv_obj_align(caption, LV_ALIGN_TOP_LEFT, 4, 0);

    // ยอดเงิน - ตัวเลขใหญ่สุดบนหน้าจอ อ่านได้จากระยะไกล
    lv_obj_t *currency = lv_label_create(left);
    lv_label_set_text(currency, "THB");
    lv_obj_set_style_text_font(currency, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(currency, COL_MUTED, 0);
    lv_obj_align(currency, LV_ALIGN_TOP_LEFT, 4, 34);

    payment_amount_label = lv_label_create(left);
    lv_label_set_text(payment_amount_label, "0");
    lv_obj_set_style_text_font(payment_amount_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(payment_amount_label, COL_TEXT, 0);
    lv_label_set_long_mode(payment_amount_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(payment_amount_label, 228);
    lv_obj_align(payment_amount_label, LV_ALIGN_TOP_LEFT, 4, 54);

    // เส้นใต้ยอดเงิน - บอกว่าช่องนี้กำลังรับค่าอยู่
    lv_obj_t *rule = lv_obj_create(left);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, 150, 3);
    lv_obj_align(rule, LV_ALIGN_TOP_LEFT, 4, 118);
    lv_obj_set_style_radius(rule, 2, 0);
    lv_obj_set_style_bg_color(rule, COL_ACCENT, 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);

    // ปุ่มยกเลิก - ล้างยอดกลับเป็น 0 (แบบ outline ให้เบากว่าปุ่มยืนยัน)
    lv_obj_t *btn_clear = lv_btn_create(left);
    lv_obj_set_size(btn_clear, 228, 46);
    lv_obj_align(btn_clear, LV_ALIGN_BOTTOM_LEFT, 4, -60);
    lv_obj_add_event_cb(btn_clear, clear_amount_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_opa(btn_clear, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_clear, 1, 0);
    lv_obj_set_style_border_color(btn_clear, COL_BORDER, 0);
    lv_obj_set_style_radius(btn_clear, 12, 0);
    lv_obj_set_style_shadow_width(btn_clear, 0, 0);
    lv_obj_set_style_bg_color(btn_clear, COL_SURFACE_HI, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_clear, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_t *lbl_clear = lv_label_create(btn_clear);
    lv_label_set_text(lbl_clear, "ยกเลิก");
    lv_obj_set_style_text_font(lbl_clear, &sarabun_20, 0);
    lv_obj_set_style_text_color(lbl_clear, COL_MUTED, 0);
    lv_obj_center(lbl_clear);

    // ปุ่มยืนยัน - เป็น primary action สีเด่นที่สุดในหน้าจอ
    lv_obj_t *btn_confirm = lv_btn_create(left);
    lv_obj_set_size(btn_confirm, 228, 50);
    lv_obj_align(btn_confirm, LV_ALIGN_BOTTOM_LEFT, 4, 0);
    lv_obj_add_event_cb(btn_confirm, numpad_bottom_btn_event_cb, LV_EVENT_CLICKED, (void *)"confirm");
    lv_obj_set_style_bg_color(btn_confirm, COL_ACCENT, 0);
    lv_obj_set_style_bg_color(btn_confirm, COL_ACCENT_DIM, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn_confirm, 12, 0);
    lv_obj_set_style_shadow_width(btn_confirm, 18, 0);
    lv_obj_set_style_shadow_color(btn_confirm, COL_ACCENT, 0);
    lv_obj_set_style_shadow_opa(btn_confirm, LV_OPA_30, 0);
    lv_obj_set_style_shadow_ofs_y(btn_confirm, 4, 0);
    lv_obj_t *lbl_confirm = lv_label_create(btn_confirm);
    lv_label_set_text(lbl_confirm, "ยืนยัน  " LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(lbl_confirm, &sarabun_20, 0);
    lv_obj_set_style_text_color(lbl_confirm, lv_color_hex(0x06231F), 0);
    lv_obj_center(lbl_confirm);

    // ---------- ฝั่งขวา: แป้นตัวเลข (แสดงตลอด ไม่ต้องกดเรียก) ----------
    static const char *keypad_map[] = {
        "1", "2", "3", "\n",
        "4", "5", "6", "\n",
        "7", "8", "9", "\n",
        "00", "0", LV_SYMBOL_BACKSPACE, ""};

    lv_obj_t *keypad = lv_btnmatrix_create(screen);
    lv_obj_set_size(keypad, 196, 268);
    lv_obj_align(keypad, LV_ALIGN_TOP_RIGHT, -16, 40);
    lv_btnmatrix_set_map(keypad, keypad_map);
    lv_obj_add_event_cb(keypad, keypad_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_style(keypad, &style_numpad_bg, 0);
    lv_obj_add_style(keypad, &style_numpad_btn, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keypad, COL_ACCENT, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(keypad, lv_color_hex(0x06231F), LV_PART_ITEMS | LV_STATE_PRESSED);

    lv_scr_load(screen);
    // ลบ screen เก่าแบบ async กันปัญหา use-after-free กรณีฟังก์ชันนี้ถูกเรียกจาก event callback
    // ของ object ที่อยู่บน screen เก่านั้นเอง (เช่นปุ่ม cancel/clear) ซึ่งยังทำงานไม่เสร็จ
    if (old_screen != NULL)
    {
        lv_obj_del_async(old_screen);
    }
}

void create_qr_payment_screen(const char *qr_data, const char *payment_intent_id, int amount)
{
    // 1. สร้าง Screen พื้นเข้ม
    lv_obj_t *screen_qr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_qr, COL_BG, 0);
    lv_obj_set_style_bg_grad_color(screen_qr, lv_color_hex(0x0C1014), 0);
    lv_obj_set_style_bg_grad_dir(screen_qr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_pad_all(screen_qr, 0, 0);
    lv_obj_clear_flag(screen_qr, LV_OBJ_FLAG_SCROLLABLE);

    // 2. ซ้าย: การ์ดสีขาวรอง QR
    //    QR ต้องอยู่บนพื้นขาวเสมอ ห้ามวางบนพื้นเข้ม ไม่งั้นกล้องมือถืออ่านไม่ติด
    lv_obj_t *qr_card = lv_obj_create(screen_qr);
    lv_obj_remove_style_all(qr_card);
    lv_obj_set_size(qr_card, 212, 212);
    lv_obj_align(qr_card, LV_ALIGN_LEFT_MID, 24, -6);
    lv_obj_clear_flag(qr_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(qr_card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(qr_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(qr_card, 16, 0);
    lv_obj_set_style_shadow_width(qr_card, 24, 0);
    lv_obj_set_style_shadow_color(qr_card, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(qr_card, LV_OPA_50, 0);
    lv_obj_set_style_shadow_ofs_y(qr_card, 6, 0);

    lv_obj_t *qr_code = lv_qrcode_create(qr_card, 180, lv_color_black(), lv_color_white());
    lv_qrcode_update(qr_code, qr_data, strlen(qr_data));
    lv_obj_center(qr_code);

    // 3. ขวา: ข้อมูลรายการ
    lv_obj_t *title_label = lv_label_create(screen_qr);
    lv_label_set_text(title_label, "สแกนเพื่อชำระเงิน");
    lv_obj_set_style_text_font(title_label, &sarabun_28, 0);
    lv_obj_set_style_text_color(title_label, COL_TEXT, 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 268, 44);

    lv_obj_t *amount_caption = lv_label_create(screen_qr);
    lv_label_set_text(amount_caption, "ยอดชำระ (บาท)");
    lv_obj_set_style_text_font(amount_caption, &sarabun_20, 0);
    lv_obj_set_style_text_color(amount_caption, COL_MUTED, 0);
    lv_obj_align(amount_caption, LV_ALIGN_TOP_LEFT, 268, 88);

    // ยอดเงินเป็นตัวเลขล้วน ใช้ montserrat ได้ ไม่ต้องพึ่งฟอนต์ไทย
    lv_obj_t *amount_label = lv_label_create(screen_qr);
    lv_label_set_text_fmt(amount_label, "%d.00", amount);
    lv_obj_set_style_text_font(amount_label, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(amount_label, COL_ACCENT, 0);
    lv_obj_align(amount_label, LV_ALIGN_TOP_LEFT, 268, 112);

    // ป้ายนับถอยหลัง - พื้นหลังจาง ๆ ให้ดูเป็น chip
    lv_obj_t *timer_chip = lv_obj_create(screen_qr);
    lv_obj_remove_style_all(timer_chip);
    lv_obj_set_size(timer_chip, 186, 38);
    lv_obj_align(timer_chip, LV_ALIGN_TOP_LEFT, 268, 168);
    lv_obj_clear_flag(timer_chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(timer_chip, COL_SURFACE, 0);
    lv_obj_set_style_bg_opa(timer_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(timer_chip, 19, 0);
    lv_obj_set_style_border_width(timer_chip, 1, 0);
    lv_obj_set_style_border_color(timer_chip, COL_BORDER, 0);

    lv_obj_t *countdown_label = lv_label_create(timer_chip);
    lv_label_set_text(countdown_label, "เหลือเวลา: 02:00");
    lv_obj_set_style_text_font(countdown_label, &sarabun_20, 0);
    lv_obj_set_style_text_color(countdown_label, COL_TEXT, 0);
    lv_obj_center(countdown_label);

    countdown_label_global = countdown_label;

    // 4. ปุ่มยกเลิกรายการ - แบบ outline ให้เบา ไม่แย่งความสนใจจาก QR
    lv_obj_t *cancel_btn = lv_btn_create(screen_qr);
    lv_obj_set_size(cancel_btn, 186, 46);
    lv_obj_align(cancel_btn, LV_ALIGN_TOP_LEFT, 268, 220);
    lv_obj_add_event_cb(cancel_btn, cancel_payment_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_opa(cancel_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cancel_btn, 1, 0);
    lv_obj_set_style_border_color(cancel_btn, COL_DANGER, 0);
    lv_obj_set_style_radius(cancel_btn, 12, 0);
    lv_obj_set_style_shadow_width(cancel_btn, 0, 0);
    lv_obj_set_style_bg_color(cancel_btn, COL_DANGER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(cancel_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "ยกเลิกรายการ");
    lv_obj_set_style_text_font(cancel_label, &sarabun_20, 0);
    lv_obj_set_style_text_color(cancel_label, COL_DANGER, 0);
    lv_obj_set_style_text_color(cancel_label, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_center(cancel_label);

    // 6. โหลดหน้าจอและเริ่มการทำงานเบื้องหลัง
    lv_scr_load(screen_qr);
    int *time_data = (int *)malloc(sizeof(int));
    *time_data = 120;
    qr_countdown_timer = lv_timer_create(qr_countdown_timer_cb, 1000, time_data);

    char *p_intent_id = (char *)malloc(strlen(payment_intent_id) + 1);
    strcpy(p_intent_id, payment_intent_id);
    xTaskCreate(check_payment_status_task, "CheckStatusTask", 4096, p_intent_id, 5, &payment_check_task_handle);
}

void show_result_screen(bool success, const char *custom_success_msg)
{
    lv_obj_t *result_overlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(result_overlay);
    lv_obj_set_size(result_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(result_overlay, COL_BG, 0);
    lv_obj_set_style_bg_opa(result_overlay, LV_OPA_90, 0);
    lv_obj_center(result_overlay);
    lv_obj_clear_flag(result_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_color_t tone = success ? COL_ACCENT : COL_DANGER;

    // วงกลมเรืองแสงรอบไอคอน ทำให้ผลลัพธ์อ่านได้ในเสี้ยววินาที
    lv_obj_t *badge = lv_obj_create(result_overlay);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, 96, 96);
    lv_obj_align(badge, LV_ALIGN_CENTER, 0, -34);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(badge, tone, 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_20, 0);
    lv_obj_set_style_border_width(badge, 2, 0);
    lv_obj_set_style_border_color(badge, tone, 0);
    lv_obj_set_style_shadow_width(badge, 40, 0);
    lv_obj_set_style_shadow_color(badge, tone, 0);
    lv_obj_set_style_shadow_opa(badge, LV_OPA_40, 0);

    lv_obj_t *icon = lv_label_create(badge);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(icon, tone, 0);
    lv_label_set_text(icon, success ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE);
    lv_obj_center(icon);

    lv_obj_t *label = lv_label_create(result_overlay);
    lv_obj_set_style_text_font(label, &sarabun_28, 0);
    lv_obj_set_style_text_color(label, COL_TEXT, 0);
    if (success && custom_success_msg != NULL && strlen(custom_success_msg) > 0)
        lv_label_set_text(label, custom_success_msg);
    else
        lv_label_set_text(label, success ? "ชำระเงินสำเร็จ" : "หมดเวลาทำรายการ");
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 40);

    lv_obj_t *hint = lv_label_create(result_overlay);
    lv_obj_set_style_text_font(hint, &sarabun_20, 0);
    lv_obj_set_style_text_color(hint, COL_MUTED, 0);
    lv_label_set_text(hint, "กำลังกลับสู่หน้าหลัก...");
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 78);

    lv_timer_create([](lv_timer_t *t)
                    {
        lv_obj_del((lv_obj_t*)t->user_data);
        create_main_payment_screen();
        lv_timer_del(t); }, 5000, result_overlay);
}

static void qr_countdown_timer_cb(lv_timer_t *timer)
{
    int *time_left = (int *)timer->user_data;
    (*time_left)--;
    if (lv_obj_is_valid(countdown_label_global))
    {
        lv_label_set_text_fmt(countdown_label_global, "เหลือเวลา: %02d:%02d", (*time_left) / 60, (*time_left) % 60);
    }
    if (*time_left < 0)
    {
        lv_timer_pause(timer);
    }
}

static void cancel_payment_event_cb(lv_event_t *e)
{
    if (payment_check_task_handle != NULL)
    {
        vTaskDelete(payment_check_task_handle);
        payment_check_task_handle = NULL;
    }

    if (qr_countdown_timer != NULL)
    {
        int *time_data = (int *)qr_countdown_timer->user_data;
        lv_timer_del(qr_countdown_timer);
        qr_countdown_timer = NULL;
        if (time_data)
        {
            free(time_data);
        }
    }
    create_main_payment_screen();
}

// =================================================================
//   PAYMENT AUDIO (แจ้งด้วยเสียงตอนชำระเงินสำเร็จ ผ่านลำโพง I2S)
// =================================================================

// ดึงเฉพาะ PCM data chunk จากไฟล์ WAV แบบไม่ยึดตำแหน่ง byte ตายตัว (เหมือนฝั่ง backend
// เผื่อมี metadata chunk แทรกก่อน data ทำให้ header ยาวไม่เท่ากัน)
static bool find_wav_data_chunk(uint8_t *wav_bytes, int wav_size, uint8_t **out_data, int *out_len)
{
    if (wav_size < 12 || memcmp(wav_bytes, "RIFF", 4) != 0 || memcmp(wav_bytes + 8, "WAVE", 4) != 0)
    {
        return false;
    }
    int pos = 12;
    while (pos + 8 <= wav_size)
    {
        uint32_t chunk_size;
        memcpy(&chunk_size, wav_bytes + pos + 4, 4);
        if (memcmp(wav_bytes + pos, "data", 4) == 0)
        {
            if (pos + 8 + (int)chunk_size > wav_size)
            {
                chunk_size = wav_size - pos - 8; // กันไฟล์ขาด
            }
            *out_data = wav_bytes + pos + 8;
            *out_len = (int)chunk_size;
            return true;
        }
        pos += 8 + (int)chunk_size + ((int)chunk_size % 2);
    }
    return false;
}

// เล่น PCM 16-bit mono ผ่าน I2S — ขยายเป็น stereo (ก็อปปี้ช่องซ้าย=ขวา) เพื่อให้เข้ากับ I2S amp
// ส่วนใหญ่ที่มักคาดหวังข้อมูลแบบ stereo แม้เนื้อเสียงจะเป็น mono ก็ตาม
static void play_pcm16_mono_via_i2s(const int16_t *samples, int sample_count, int sample_rate)
{
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = (uint32_t)sample_rate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = 512,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0,
    };
    i2s_pin_config_t pin_config = {
        .mck_io_num = I2S_PIN_NO_CHANGE,
        .bck_io_num = AUDIO_I2S_BCK_IO,
        .ws_io_num = AUDIO_I2S_LRCK_IO,
        .data_out_num = AUDIO_I2S_DO_IO,
        .data_in_num = I2S_PIN_NO_CHANGE,
    };

    if (i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL) != ESP_OK)
    {
        Serial.println("PaymentAudio: i2s_driver_install failed");
        return;
    }
    i2s_set_pin(I2S_NUM_0, &pin_config);

    const int CHUNK = 512;
    int16_t stereo_buf[CHUNK * 2];
    size_t bytes_written;
    for (int i = 0; i < sample_count; i += CHUNK)
    {
        int n = (i + CHUNK <= sample_count) ? CHUNK : (sample_count - i);
        for (int j = 0; j < n; j++)
        {
            stereo_buf[j * 2] = samples[i + j];
            stereo_buf[j * 2 + 1] = samples[i + j];
        }
        i2s_write(I2S_NUM_0, stereo_buf, n * 2 * sizeof(int16_t), &bytes_written, portMAX_DELAY);
    }

    i2s_driver_uninstall(I2S_NUM_0);
}

// ดาวน์โหลดไฟล์เสียงแจ้งยอดชำระจาก backend แล้วเล่นผ่านลำโพง — รันใน task แยกไม่ให้บล็อกงานอื่น
void play_payment_audio_task(void *pvParameters)
{
    int amount = (int)(intptr_t)pvParameters;

    preferences.begin("paybox-cfg", true);
    String ttsUrlBase = preferences.getString(P_TTS_URL, "https://ttmb-tech.com/paybox-api/tts_payment.php?key=eac86d7a2a8faf09051ac70d70d20901&amount=");
    preferences.end();

    if (ttsUrlBase.length() == 0)
    {
        vTaskDelete(NULL);
        return;
    }

    String fullUrl = ttsUrlBase + String(amount);
    HTTPClient http_tts;
    if (http_tts.begin(fullUrl))
    {
        int httpCode = http_tts.GET();
        if (httpCode == HTTP_CODE_OK)
        {
            int len = http_tts.getSize();
            if (len > 44 && len < 2 * 1024 * 1024)
            {
                uint8_t *wav_buf = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
                if (wav_buf != NULL)
                {
                    WiFiClient *stream = http_tts.getStreamPtr();
                    int total_read = 0;
                    unsigned long start_time = millis();
                    while (total_read < len && http_tts.connected() && (millis() - start_time) < 15000)
                    {
                        int avail = stream->available();
                        if (avail > 0)
                        {
                            int to_read = avail < (len - total_read) ? avail : (len - total_read);
                            total_read += stream->readBytes(wav_buf + total_read, to_read);
                        }
                        else
                        {
                            vTaskDelay(pdMS_TO_TICKS(5));
                        }
                    }

                    if (total_read == len)
                    {
                        uint8_t *pcm_data = NULL;
                        int pcm_len = 0;
                        if (find_wav_data_chunk(wav_buf, len, &pcm_data, &pcm_len) && pcm_len > 0)
                        {
                            play_pcm16_mono_via_i2s((const int16_t *)pcm_data, pcm_len / 2, 16000);
                        }
                        else
                        {
                            Serial.println("PaymentAudio: could not find WAV data chunk");
                        }
                    }
                    else
                    {
                        Serial.printf("PaymentAudio: download incomplete (%d/%d bytes)\n", total_read, len);
                    }
                    heap_caps_free(wav_buf);
                }
            }
            else
            {
                Serial.printf("PaymentAudio: invalid content length %d\n", len);
            }
        }
        else
        {
            Serial.printf("PaymentAudio: HTTP GET failed, code=%d\n", httpCode);
        }
        http_tts.end();
    }

    vTaskDelete(NULL);
}

// =================================================================
//   OTA FIRMWARE UPDATE
// =================================================================

// เช็คเวอร์ชันใหม่จาก backend เป็นระยะๆ ถ้ามีใหม่กว่าปัจจุบันให้โหลด+แฟลชแล้ว reboot อัตโนมัติ
void ota_check_task(void *pvParameters)
{
    char *ota_url_base = (char *)pvParameters;

    // รอให้ผ่านช่วงบูตที่มีงานอื่นแย่ง DRAM เยอะไปก่อน (banner fetch, network task ฯลฯ)
    // ลดโอกาส TLS handshake ล้มเหลวเพราะจองหน่วยความจำไม่ได้
    vTaskDelay(pdMS_TO_TICKS(15000));

    while (true)
    {
        HTTPClient http_ota;
        String full_url = String(ota_url_base) + FIRMWARE_VERSION;
        if (http_ota.begin(full_url))
        {
            int httpCode = http_ota.GET();
            if (httpCode == HTTP_CODE_OK)
            {
                String payload = http_ota.getString();
                DynamicJsonDocument doc(512);
                DeserializationError err = deserializeJson(doc, payload);
                if (!err && doc["update_available"] == true && doc.containsKey("url"))
                {
                    String bin_url = doc["url"].as<String>();
                    String new_version = doc["version"] | "unknown";
                    http_ota.end();

                    Serial.printf("OTA: update available (%s -> %s), downloading...\n", FIRMWARE_VERSION, new_version.c_str());

                    WiFiClientSecure ota_client;
                    ota_client.setInsecure(); // ไม่ pin certificate เหมือน endpoint อื่นในระบบนี้
                    httpUpdate.rebootOnUpdate(true);
                    t_httpUpdate_return ret = httpUpdate.update(ota_client, bin_url);
                    switch (ret)
                    {
                    case HTTP_UPDATE_FAILED:
                        Serial.printf("OTA: update failed (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
                        break;
                    case HTTP_UPDATE_NO_UPDATES:
                        Serial.println("OTA: no updates (unexpected — server said update was available)");
                        break;
                    case HTTP_UPDATE_OK:
                        Serial.println("OTA: update installed, rebooting...");
                        break;
                    }
                    // ถ้าสำเร็จ อุปกรณ์ reboot เองแล้ว (rebootOnUpdate(true)) โค้ดหลังจุดนี้จะไม่ทำงาน
                }
                else
                {
                    http_ota.end();
                }
            }
            else
            {
                Serial.printf("OTA: check failed, HTTP code=%d\n", httpCode);
                http_ota.end();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(6UL * 60UL * 60UL * 1000UL)); // เช็คซ้ำทุก 6 ชั่วโมง
    }
}

void check_payment_status_task(void *pvParameters)
{
    char *payment_intent_id = (char *)pvParameters;
    String check_status_url;
    preferences.begin("paybox-cfg", true);
    check_status_url = preferences.getString(P_PAY_CHECK_STATUS, "");
    preferences.end();
    unsigned long startTime = millis();
    bool payment_succeeded = false;
    while (millis() - startTime < 120000)
    {
        if (WiFi.status() == WL_CONNECTED)
        {
            HTTPClient http_check;
            String full_url = check_status_url + String(payment_intent_id);
            if (http_check.begin(full_url))
            {
                int httpCode = http_check.GET();
                if (httpCode == HTTP_CODE_OK)
                {
                    String payload = http_check.getString();
                    DynamicJsonDocument doc(1024);
                    deserializeJson(doc, payload);
                    if (doc["success"] == true && strcmp(doc["status"], "succeeded") == 0)
                    {
                        payment_succeeded = true;
                    }
                }
                http_check.end();
            }
        }
        if (payment_succeeded)
        {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    // ทำงานตาม Operating Mode หลังชำระเงินสำเร็จ (Pulse / Thank You / Payment)
    String result_message = "";
    if (payment_succeeded)
    {
        // แจ้งด้วยเสียงว่าได้รับเงินจำนวนเท่าไหร่ — รันแยก task กันบล็อกงานอื่น (Pulse/ThankYou/แสดงผล)
        xTaskCreate(play_payment_audio_task, "PaymentAudio", 8192, (void *)(intptr_t)payment_amount, 1, NULL);

        preferences.begin("paybox-cfg", true);
        int op_mode = preferences.getInt(P_OP_MODE, 3);
        preferences.end();

        switch (op_mode)
        {
        case 1: // Pulse Mode: ดีเลย์สั้นๆ แล้วส่งสัญญาณ pulse ออก GPIO ที่ตั้งค่าไว้
        {
            preferences.begin("paybox-cfg", true);
            int pulse_pin = preferences.getInt(P_PULSE_PIN, 14);
            preferences.end();
            vTaskDelay(pdMS_TO_TICKS(500));
            pinMode(pulse_pin, OUTPUT);
            digitalWrite(pulse_pin, HIGH);
            vTaskDelay(pdMS_TO_TICKS(500));
            digitalWrite(pulse_pin, LOW);
            break;
        }
        case 2: // Thank You Mode: เรียก API ที่ตั้งไว้ (แทน {MAC}) แล้วแสดงข้อความที่ตั้งไว้
        {
            preferences.begin("paybox-cfg", true);
            String ty_api = preferences.getString(P_THANKYOU_API, "");
            String ty_msg = preferences.getString(P_THANKYOU_MSG, "Thank You!");
            preferences.end();
            result_message = ty_msg;

            if (ty_api.length() > 0 && WiFi.status() == WL_CONNECTED)
            {
                ty_api.replace("{MAC}", WiFi.macAddress());
                HTTPClient http_ty;
                if (http_ty.begin(ty_api))
                {
                    http_ty.GET();
                    http_ty.end();
                }
            }
            break;
        }
        default: // Payment Mode: ใช้ข้อความ Thank You ที่ตั้งค่าไว้สำหรับหน้าชำระเงิน
        {
            preferences.begin("paybox-cfg", true);
            result_message = preferences.getString(P_PAY_THANKYOU_MSG, "");
            preferences.end();
            break;
        }
        }
    }

    if (lvgl_port_lock(0))
    {

        if (qr_countdown_timer != NULL)
        {
            int *time_data = (int *)qr_countdown_timer->user_data; // แก้ไขตรงนี้

            lv_timer_del(qr_countdown_timer);
            qr_countdown_timer = NULL;
            if (time_data)
            {
                free(time_data);
            }
        }

        show_result_screen(payment_succeeded, result_message.c_str());
        lvgl_port_unlock();
    }
    free(payment_intent_id);
    payment_check_task_handle = NULL;
    vTaskDelete(NULL);
}

// =================================================================
//   LVGL EVENT CALLBACKS
// =================================================================
static void plus_btn_event_cb(lv_event_t *e)
{
    preferences.begin("paybox-cfg", true);
    int increment = preferences.getInt(P_PAY_INCREMENT, 10);
    preferences.end();

    payment_amount += increment;
    lv_label_set_text_fmt(payment_amount_label, "%d", payment_amount);
}

static void minus_btn_event_cb(lv_event_t *e)
{
    preferences.begin("paybox-cfg", true);
    int increment = preferences.getInt(P_PAY_INCREMENT, 10);
    preferences.end();
    payment_amount -= increment;
    if (payment_amount < 0)
    {
        payment_amount = 0;
    }
    lv_label_set_text_fmt(payment_amount_label, "%d", payment_amount);
}

void show_loading_spinner()
{
    if (lvgl_port_lock(10))
    {
        lv_obj_t *overlay = lv_obj_create(lv_scr_act());
        lv_obj_remove_style_all(overlay);
        lv_obj_set_size(overlay, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
        lv_obj_center(overlay);

        lv_obj_t *spinner = lv_spinner_create(overlay, 1000, 60);
        lv_obj_set_size(spinner, 80, 80);
        lv_obj_center(spinner);

        lv_obj_t *label = lv_label_create(overlay);
        lv_label_set_text(label, "กำลังสร้าง QR Code...");
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_set_style_text_font(label, &sarabun_20, 0);
        lv_obj_align_to(label, spinner, LV_ALIGN_OUT_BOTTOM_MID, 0, 15);

        lvgl_port_unlock();
    }
}

static void confirm_event_cb(lv_event_t *e)
{
    if (payment_amount > 0)
    {
        show_loading_spinner();
        NetworkRequest req;
        req.amount = payment_amount;
        xQueueSend(network_queue, &req, portMAX_DELAY);
    }
}

// =================================================================
//   BACKGROUND NETWORK TASK
// =================================================================

void network_task(void *pvParameters)
{
    NetworkRequest received_req;
    String gen_qr_url, check_status_url;

    preferences.begin("paybox-cfg", true);
    gen_qr_url = preferences.getString(P_PAY_GEN_QR, "");
    check_status_url = preferences.getString(P_PAY_CHECK_STATUS, "");
    preferences.end();

    while (1)
    {
        if (xQueueReceive(network_queue, &received_req, portMAX_DELAY))
        {
            HTTPClient http;
            String full_qr_url = gen_qr_url + String(received_req.amount);
            bool success = false;
            char *qr_data_buffer = NULL;
            char *intent_id_buffer = NULL;

            Serial.printf("URL to generate QR: %s\n", full_qr_url.c_str());
            if (http.begin(full_qr_url))
            {
                int httpCode = http.GET();

                if (httpCode == HTTP_CODE_OK)
                {
                    String payload = http.getString();
                    DynamicJsonDocument doc(1024);
                    deserializeJson(doc, payload);

                    if (doc["success"] == true && doc.containsKey("qrCodeRawData"))
                    {
                        // 1. จองหน่วยความจำใหม่สำหรับข้อมูล QR
                        const char *qr_data_str = doc["qrCodeRawData"];
                        qr_data_buffer = (char *)malloc(strlen(qr_data_str) + 1);
                        if (qr_data_buffer)
                            strcpy(qr_data_buffer, qr_data_str);

                        // 2. จองหน่วยความจำใหม่สำหรับ Payment Intent ID
                        const char *intent_id_str = doc["paymentIntentId"];
                        intent_id_buffer = (char *)malloc(strlen(intent_id_str) + 1);
                        if (intent_id_buffer)
                            strcpy(intent_id_buffer, intent_id_str);
                        success = true;
                    }
                }
                http.end();
            }

            // ล็อค LVGL เพื่ออัปเดตหน้าจอ
            if (lvgl_port_lock(0))
            {
                if (success && qr_data_buffer != NULL && intent_id_buffer != NULL)
                {
                    // 3. ส่ง Pointer ที่ปลอดภัยไปให้ฟังก์ชันสร้าง UI
                    create_qr_payment_screen(qr_data_buffer, intent_id_buffer, received_req.amount);
                }
                else
                {
                    create_main_payment_screen();
                }
                lvgl_port_unlock();
            }

            // 4. คืนค่าหน่วยความจำที่เราจองไว้สำหรับ qr_data
            // (เพราะ lv_qrcode_update ได้คัดลอกข้อมูลไปเก็บไว้ข้างในแล้ว)
            if (qr_data_buffer != NULL)
            {
                free(qr_data_buffer);
            }

            // หมายเหตุ: intent_id_buffer ไม่ต้อง free ที่นี่
            // เพราะจะถูกส่งต่อให้ check_payment_status_task ซึ่งจะ free เองเมื่อทำงานเสร็จ
        }
    }
}

void main_app_task(void *pvParameters)
{
    bool is_configured = false;
    preferences.begin("paybox-cfg", true);
    is_configured = preferences.getBool(P_CONFIGURED, false);
    preferences.end();

    if (!is_configured)
    {

        current_app_state = APP_STATE_SETUP;
        create_ui_setup_ap_screen();
        WiFi.softAP("357Paybox", NULL);
        IPAddress apIP(192, 168, 5, 1);
        WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
        xTaskCreate(web_server_task, "WebServerTask", 8192, NULL, 1, NULL);
    }
    else
    {
        current_app_state = APP_STATE_CONNECTING;
        preferences.begin("paybox-cfg", true);
        String ssid = preferences.getString(P_WIFI_SSID, "");
        String pass = preferences.getString(P_WIFI_PASS, "");
        int op_mode = preferences.getInt(P_OP_MODE, 0);
        preferences.end();
        create_ui_connecting_wifi_screen(ssid.c_str());
        WiFi.mode(WIFI_STA);
        // ปิด WiFi power-save ฝั่ง ESP32 — ถ้าเปิดไว้ (ค่า default) จะชนกับ power-saving ของ
        // iPhone Personal Hotspot (และ router บางรุ่น) ทำให้หลุด-ต่อ WiFi วนซ้ำๆ ทั้งที่สัญญาณปกติดี
        WiFi.setSleep(false);
        Serial.printf("Connecting to WiFi SSID='%s'...\n", ssid.c_str());
        WiFi.begin(ssid.c_str(), pass.c_str());
        uint8_t wifi_result = WiFi.waitForConnectResult(30000);
        Serial.printf("WiFi.waitForConnectResult() returned status=%d\n", (int)wifi_result);
        if (wifi_result == WL_CONNECTED)
        {
            Serial.printf("WiFi connected, IP=%s\n", WiFi.localIP().toString().c_str());
            current_app_state = APP_STATE_RUNNING;

            preferences.begin("paybox-cfg", true);
            String otaUrl = preferences.getString(P_OTA_URL, "https://ttmb-tech.com/paybox-api/firmware_check.php?key=eac86d7a2a8faf09051ac70d70d20901&version=");
            preferences.end();
            if (otaUrl.length() > 0)
            {
                char *otaUrlCopy = strdup(otaUrl.c_str());
                xTaskCreate(ota_check_task, "OtaCheck", 8192, otaUrlCopy, 1, NULL);
            }

            preferences.begin("paybox-cfg", true);
            // เว้นค่า default ไว้เฉพาะ slot แรกเพื่อทดสอบ ส่วน slot 2-5 เป็น placeholder ตัวอย่าง
            // (เข้าหน้า setup แล้วเปลี่ยนเป็น URL จริงของแต่ละร้านได้ทีหลัง)
            static const char *default_banner_urls[MAX_BANNERS] = {
                "https://ttmb-tech.com/paybox-api/banner1.png",
                "https://placehold.co/480x320/1a73e8/white.png?text=Banner+2",
                "https://placehold.co/480x320/e91e63/white.png?text=Banner+3",
                "https://placehold.co/480x320/34a853/white.png?text=Banner+4",
                "https://placehold.co/480x320/fbbc05/333333.png?text=Banner+5",
            };
            String *bannerUrls = new String[MAX_BANNERS];
            for (int i = 0; i < MAX_BANNERS; i++)
            {
                bannerUrls[i] = preferences.getString(P_BANNER_URL[i], default_banner_urls[i]);
            }
            int bannerIdleSec = preferences.getInt(P_BANNER_IDLE_SEC, 20);
            preferences.end();
            banner_idle_ms = (uint32_t)(bannerIdleSec > 0 ? bannerIdleSec : 20) * 1000;
            xTaskCreate(banner_fetch_task, "BannerFetch", 12288, bannerUrls, 1, NULL);

            if (lvgl_port_lock(0))
            {
                switch (op_mode)
                {
                case 1: // Pulse Mode: ทำ flow ชำระเงินเหมือนกัน แต่หลังสำเร็จจะส่ง pulse ออก GPIO แทน
                case 2: // Thank You Mode: ทำ flow ชำระเงินเหมือนกัน แต่หลังสำเร็จจะเรียก API + โชว์ข้อความ Thank You แทน
                case 3: // Payment Mode: flow ชำระเงินปกติ
                    create_main_payment_screen();
                    xTaskCreate(network_task, "NetworkTask", 10240, NULL, 2, NULL);
                    lv_timer_create(idle_check_timer_cb, 1000, NULL);
                    break;
                default:
                    break;
                }
                lvgl_port_unlock();
            }
        }
        else
        {
            Serial.println("WiFi connect failed/timed out. Clearing config and restarting...");
            preferences.begin("paybox-cfg", false);
            preferences.clear();
            preferences.end();
            ESP.restart();
        }
    }
    vTaskDelete(NULL);
}

void setup()
{
    Serial.begin(115200);
    Serial.println("Starting 357Paybox...");
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = EXAMPLE_LCD_QSPI_H_RES * EXAMPLE_LCD_QSPI_V_RES * 4,
        .rotate = LV_DISP_ROT_90,
    };

    cfg.lvgl_port_cfg.task_stack = 8192;
    cfg.lvgl_port_cfg.task_priority = 5;
    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();
    network_queue = xQueueCreate(5, sizeof(NetworkRequest));
    xTaskCreate(main_app_task, "MainAppTask", 8192, NULL, 2, NULL);
    Serial.println("Setup complete. Handing over to FreeRTOS tasks.");
}

void loop()
{
    // bsp_display_start_with_config() สร้าง task ภายใน (lvgl_port_task ใน lv_port.c) ที่เรียก
    // lv_timer_handler() เองอยู่แล้วโดยใช้ lock ของตัวเอง (lvgl_port_lock/unlock) — เดิมโค้ดตรงนี้เรียก
    // lv_timer_handler() ซ้ำอีกรอบโดยใช้ mutex คนละตัว (lvgl_mutex) ทำให้สอง task เข้าถึง LVGL
    // พร้อมกันแบบไม่ synchronize กัน (LVGL ไม่ thread-safe) เป็นสาเหตุของอาการจอค้าง/render ไม่ครบ
    // ที่เจอแบบสุ่มๆ — เอาออกแล้วปล่อยให้ lvgl_port_task จัดการ refresh คนเดียว
    vTaskDelay(pdMS_TO_TICKS(100));
}