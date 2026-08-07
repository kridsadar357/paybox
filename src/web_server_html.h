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
  .mode-settings { display: none; margin-top: 20px; padding: 20px; border-top: 1px solid #e5e7eb; background: #fafafa; border-radius: 6px; }
  #loading { font-weight: bold; color: #ef4444; }
  small { color: #6b7280; }
</style>
<script>
function showModeSettings() {
  document.querySelectorAll('.mode-settings').forEach(el => el.style.display = 'none');
  const selectedMode = document.getElementById('op_mode').value;
  if (selectedMode) {
    document.getElementById('mode_' + selectedMode).style.display = 'block';
  }
}
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
    <h2>1. WiFi Connection</h2>
    <label for="wifi_ssid">Select WiFi Network:</label>
    <select id="wifi_ssid" onchange="handleSsidChange()"><option value="">Loading...</option></select>
    <div id="loading" style="display:none;">Scanning for networks...</div>
    <label for="ssid_text">Or Enter SSID Manually:</label>
    <input type="text" id="ssid_text" name="wifi_ssid" required>
    <label for="wifi_pass">Password:</label>
    <input type="password" name="wifi_pass">
    
    <h2>2. Select Operating Mode</h2>
    <label for="op_mode">Mode:</label>
    <select id="op_mode" name="op_mode" onchange="showModeSettings()" required>
      <option value="">-- Please choose a mode --</option>
      <option value="pulse">Trigger Pulse</option>
      <option value="thankyou">Thank You Screen</option>
      <option value="payment">Payment Screen</option>
    </select>
    
    <div id="mode_pulse" class="mode-settings">
      <h3>Trigger Pulse Settings</h3>
      <label for="pulse_pin">Output GPIO Pin:</label>
      <select name="pulse_pin">
        <option value="14">GPIO 14</option><option value="9">GPIO 9</option><option value="46">GPIO 46</option><option value="16">GPIO 16</option><option value="15">GPIO 15</option><option value="7">GPIO 7</option><option value="6">GPIO 6</option><option value="5">GPIO 5</option>
      </select>
      <label for="pulse_baht_inc">Baht per Pulse:</label>
      <input type="number" name="pulse_baht_inc" value="0" min="0">
      <small>e.g. 5 = one pulse per 5 baht paid (for coin-selector emulation). 0 = a single pulse per transaction regardless of amount.</small>
    </div>

    <div id="mode_thankyou" class="mode-settings">
      <h3>Thank You Screen Settings</h3>
      <label for="ty_api">API URL:</label>
      <input type="text" name="ty_api" placeholder="https://example.com/data.php?device={MAC}">
      <small>Use {MAC} as a placeholder for the device's MAC address.</small>
      <label for="ty_msg">Thank You Message:</label>
      <input type="text" name="ty_msg" value="Thank You!">
    </div>

    <div id="mode_payment" class="mode-settings">
      <h3>Payment Screen Settings</h3>
      <label for="pay_inc">Increment/Decrement Value:</label>
      <input type="number" name="pay_inc" value="10">
      <label for="pay_ty_msg">Thank You Message:</label>
      <input type="text" name="pay_ty_msg" value="Payment Received!">
    </div>

    <h2>3. Idle Banner Slideshow (Optional)</h2>
    <small>Shown full-screen (cycling every 5s if more than one) when the device has been idle (no touch) for the timeout below. Leave all blank to disable.</small>
    <label for="banner_url_1">Banner Image URL 1 (PNG):</label>
    <input type="text" name="banner_url_1" placeholder="https://.../banner1.png">
    <label for="banner_url_2">Banner Image URL 2 (PNG):</label>
    <input type="text" name="banner_url_2" placeholder="https://.../banner2.png">
    <label for="banner_url_3">Banner Image URL 3 (PNG):</label>
    <input type="text" name="banner_url_3" placeholder="https://.../banner3.png">
    <label for="banner_url_4">Banner Image URL 4 (PNG):</label>
    <input type="text" name="banner_url_4" placeholder="https://.../banner4.png">
    <label for="banner_url_5">Banner Image URL 5 (PNG):</label>
    <input type="text" name="banner_url_5" placeholder="https://.../banner5.png">
    <label for="banner_idle_sec">Idle Timeout (seconds):</label>
    <input type="number" name="banner_idle_sec" value="20" min="5">

    <button type="submit">Save & Reboot</button>
  </form>
</div>
</body></html>
)rawliteral";