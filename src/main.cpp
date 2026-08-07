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

// FreeRTOS
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

extern "C"
{
    LV_FONT_DECLARE(sarabun_20);
    LV_FONT_DECLARE(sarabun_28);
}
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

void create_main_payment_screen()
{
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
            if (lvgl_port_lock(0))
            {
                switch (op_mode)
                {
                case 1: // Pulse Mode: ทำ flow ชำระเงินเหมือนกัน แต่หลังสำเร็จจะส่ง pulse ออก GPIO แทน
                case 2: // Thank You Mode: ทำ flow ชำระเงินเหมือนกัน แต่หลังสำเร็จจะเรียก API + โชว์ข้อความ Thank You แทน
                case 3: // Payment Mode: flow ชำระเงินปกติ
                    create_main_payment_screen();
                    xTaskCreate(network_task, "NetworkTask", 10240, NULL, 2, NULL);
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