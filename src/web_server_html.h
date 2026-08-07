#pragma once

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML>
<html><head>
<title>357Paybox Setup</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; background: #f5f7fa; color: #333; line-height: 1.6; }
  .container { max-width: 600px; margin: 20px auto; padding: 25px; background: #fff; border-radius: 10px; box-shadow: 0 4px 12px rgba(0,0,0,0.1); }
  h1, h2 { color: #1d4ed8; border-bottom: 2px solid #eef2ff; padding-bottom: 10px; }
  label { display: block; margin-top: 18px; font-weight: 600; color: #374151; }
  input[type="text"], input[type="password"], input[type="number"], select { width: 100%; padding: 12px; margin-top: 6px; border: 1px solid #d1d5db; border-radius: 6px; box-sizing: border-box; transition: border-color 0.2s; }
  input:focus, select:focus { border-color: #3b82f6; outline: none; }
  button { background-color: #1d4ed8; color: white; padding: 14px 20px; border: none; border-radius: 6px; cursor: pointer; font-size: 16px; font-weight: 600; margin-top: 25px; width: 100%; transition: background-color 0.2s; }
  button:hover { background-color: #1e40af; }
  #loading { font-weight: bold; color: #ef4444; }
  small { color: #6b7280; }
</style>
<script>
function scanWifi() {
  const select = document.getElementById('wifi_ssid');
  const loading = document.getElementById('loading');
  select.innerHTML = '<option value="">Scanning...</option>';
  loading.style.display = 'block';
  fetch('/scan')
    .then(response => response.json())
    .then(data => {
      select.innerHTML = '<option value="">-- Select a WiFi Network --</option>';
      data.forEach(net => {
        let option = new Option(net.ssid + ' (Signal: ' + net.rssi + 'dBm)', net.ssid);
        select.add(option);
      });
      loading.style.display = 'none';
    })
    .catch(error => {
      console.error('Error scanning WiFi:', error);
      select.innerHTML = '<option value="">Scan failed, please enter manually</option>';
      loading.style.display = 'none';
    });
}
function handleSsidChange(){
    document.getElementById('ssid_text').value = document.getElementById('wifi_ssid').value;
}
window.onload = function() { scanWifi(); };
</script>
</head><body>
<div class="container">
  <h1>357Paybox Setup</h1>
  <form action="/save" method="POST">
    <h2>WiFi Connection</h2>
    <label for="wifi_ssid">Select WiFi Network:</label>
    <select id="wifi_ssid" onchange="handleSsidChange()"><option value="">Loading...</option></select>
    <div id="loading" style="display:none;">Scanning for networks...</div>
    <label for="ssid_text">Or Enter SSID Manually:</label>
    <input type="text" id="ssid_text" name="wifi_ssid" required>
    <label for="wifi_pass">Password:</label>
    <input type="password" name="wifi_pass">
    <small>ตั้งค่าอื่นๆ ทั้งหมด (โหมดรับยอด/ปุ่มพรีเซ็ต/pulse/ชื่อร้าน/banner) จัดการได้จากหน้า admin
    หลังจากอุปกรณ์เชื่อมต่อ WiFi และยืนยันตัวตนสำเร็จ ไม่ต้องตั้งค่าที่นี่</small>

    <button type="submit">Save & Reboot</button>
  </form>
</div>
</body></html>
)rawliteral";