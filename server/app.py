import os
import time
import json
from datetime import datetime
from flask import Flask, request, jsonify, Response, render_template_string, send_from_directory
app = Flask(__name__)

# File lưu danh sách thẻ RFID
RFID_DB_FILE = os.path.join(os.path.dirname(__file__), "rfid_cards.json")

def load_rfid_cards():
    if os.path.exists(RFID_DB_FILE):
        try:
            with open(RFID_DB_FILE, "r") as f:
                return json.load(f)
        except Exception:
            pass
    return ["A1 B2 C3 D4"]

def save_rfid_cards(cards):
    try:
        with open(RFID_DB_FILE, "w") as f:
            json.dump(cards, f, indent=2)
    except Exception as e:
        print(f"Error saving RFID cards: {e}")

# Trạng thái hệ thống trong bộ nhớ
device_state = {
    "device_id": "esp32s3cam",
    "pir": False,
    "gas": False,
    "door_sensor": False,
    "temp": 0.0,
    "hum": 0.0,
    "uptime": 0,
    "last_seen": 0,
    "online": False
}

device_commands = {
    "fan": 0,
    "lamp": 0,
    "door": 0,
    "buzzer": 0,
    "rfid_enroll": 0
}

latest_jpeg_frame = None
latest_frame_time = 0
rfid_whitelist = load_rfid_cards()
event_logs = []

def add_log(msg):
    ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    event_logs.insert(0, f"[{ts}] {msg}")
    if len(event_logs) > 50:
        event_logs.pop()

add_log("SYSTEM INITIALIZED")

# =========================================================================
# API DÀNH CHO ESP32-S3
# =========================================================================

@app.route('/api/device/status', methods=['POST'])
def device_status():
    global device_state
    data = request.get_json(silent=True) or {}
    device_state["pir"] = bool(data.get("pir", False))
    device_state["gas"] = bool(data.get("gas", False))
    device_state["door_sensor"] = bool(data.get("door_sensor", False))
    device_state["temp"] = float(data.get("temp", 0.0))
    device_state["hum"] = float(data.get("hum", 0.0))
    device_state["uptime"] = int(data.get("uptime", 0))
    device_state["last_seen"] = time.time()
    device_state["online"] = True
    return jsonify({"status": "OK"})

@app.route('/api/device/command', methods=['GET'])
def device_command():
    return jsonify(device_commands)

@app.route('/api/device/camera/upload', methods=['POST'])
def camera_upload():
    global latest_jpeg_frame, latest_frame_time
    latest_jpeg_frame = request.data
    latest_frame_time = time.time()
    return jsonify({"status": "OK", "bytes": len(latest_jpeg_frame)})

@app.route('/api/device/rfid/check', methods=['POST'])
def rfid_check():
    data = request.get_json(silent=True) or {}
    uid = data.get("uid", "").strip().upper()
    authorized = uid in [c.upper() for c in rfid_whitelist]
    if authorized:
        add_log(f"ACCESS GRANTED - UID: {uid}")
        device_commands["door"] = 1
    else:
        add_log(f"ACCESS DENIED - UNKNOWN UID: {uid}")
    return jsonify({"authorized": authorized})

@app.route('/api/device/rfid/enroll', methods=['POST'])
def rfid_enroll():
    data = request.get_json(silent=True) or {}
    uid = data.get("uid", "").strip().upper()
    if uid and uid not in [c.upper() for c in rfid_whitelist]:
        rfid_whitelist.append(uid)
        save_rfid_cards(rfid_whitelist)
        add_log(f"NEW RFID ENROLLED: {uid}")
        return jsonify({"status": "OK", "enrolled": uid})
    return jsonify({"status": "EXISTS_OR_INVALID"})

# =========================================================================
# API DÀNH CHO WEB DASHBOARD
# =========================================================================

@app.route('/api/web/state', methods=['GET'])
def web_state():
    is_online = (time.time() - device_state["last_seen"]) < 6
    device_state["online"] = is_online
    return jsonify({
        "device": device_state,
        "commands": device_commands,
        "rfid_cards": rfid_whitelist,
        "logs": event_logs[:20],
        "has_camera": (latest_jpeg_frame is not None and (time.time() - latest_frame_time) < 15),
        "frame_age": round(time.time() - latest_frame_time, 1) if latest_frame_time > 0 else -1
    })

@app.route('/api/web/command', methods=['POST'])
def web_command():
    data = request.get_json(silent=True) or {}
    for key in ["fan", "lamp", "door", "buzzer", "rfid_enroll"]:
        if key in data:
            val = int(data[key])
            device_commands[key] = val
            add_log(f"COMMAND SET: {key.upper()} = {val}")
    return jsonify({"status": "OK", "commands": device_commands})

@app.route('/api/web/rfid/add', methods=['POST'])
def web_rfid_add():
    data = request.get_json(silent=True) or {}
    uid = data.get("uid", "").strip().upper()
    if uid and uid not in [c.upper() for c in rfid_whitelist]:
        rfid_whitelist.append(uid)
        save_rfid_cards(rfid_whitelist)
        add_log(f"MANUAL RFID ADDED: {uid}")
        return jsonify({"status": "OK"})
    return jsonify({"status": "EXISTS_OR_INVALID"}), 400

@app.route('/api/web/rfid/delete', methods=['POST'])
def web_rfid_delete():
    data = request.get_json(silent=True) or {}
    uid = data.get("uid", "").strip().upper()
    cards_upper = [c.upper() for c in rfid_whitelist]
    if uid in cards_upper:
        idx = cards_upper.index(uid)
        removed = rfid_whitelist.pop(idx)
        save_rfid_cards(rfid_whitelist)
        add_log(f"RFID DELETED: {removed}")
        return jsonify({"status": "OK"})
    return jsonify({"status": "NOT_FOUND"}), 404

# =========================================================================
# LUỒNG VIDEO & SNAPSHOT
# =========================================================================

def generate_mjpeg_stream():
    while True:
        if latest_jpeg_frame:
            yield (b'--frame\r\n'
                   b'Content-Type: image/jpeg\r\n\r\n' + latest_jpeg_frame + b'\r\n')
        time.sleep(0.08)

@app.route('/stream')
def video_stream():
    return Response(generate_mjpeg_stream(), mimetype='multipart/x-mixed-replace; boundary=frame')

@app.route('/snapshot')
def video_snapshot():
    if latest_jpeg_frame:
        return Response(latest_jpeg_frame, mimetype='image/jpeg')
    return Response(b'', status=404)

# =========================================================================
# GIAO DIỆN NỀN TRẮNG - ĐƠN GIẢN - PHẲNG - KHÔNG ĐỔ BÓNG - KHÔNG DÙNG ICON
# =========================================================================

HTML_DASHBOARD = """<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32-S3 System</title>
    <link rel="icon" type="image/png" href="/favicon.ico?v=1">
    <style>
        * {
            box-sizing: border-box;
            margin: 0;
            padding: 0;
        }
        body {
            background-color: #ffffff;
            color: #000000;
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
            font-size: 13px;
            line-height: 1.5;
            padding: 24px 20px;
        }
        .container {
            max-width: 960px;
            margin: 0 auto;
        }
        header {
            border-bottom: 1px solid #000000;
            padding-bottom: 12px;
            margin-bottom: 24px;
            display: flex;
            justify-content: space-between;
            align-items: flex-end;
            flex-wrap: wrap;
            gap: 12px;
        }
        .title {
            font-size: 16px;
            font-weight: 700;
            letter-spacing: 0.5px;
            text-transform: uppercase;
        }
        .meta {
            font-size: 12px;
            color: #555555;
            text-align: right;
        }
        .status-badge {
            display: inline-block;
            padding: 2px 6px;
            border: 1px solid #000000;
            font-size: 11px;
            font-weight: 600;
            text-transform: uppercase;
        }
        .status-badge.online {
            background-color: #000000;
            color: #ffffff;
        }
        .status-badge.offline {
            background-color: #ffffff;
            color: #777777;
            border-color: #cccccc;
        }

        .grid {
            display: grid;
            grid-template-columns: 1fr;
            gap: 24px;
        }
        @media (min-width: 768px) {
            .grid {
                grid-template-columns: 1.15fr 1fr;
            }
        }

        .panel {
            border: 1px solid #e0e0e0;
            background-color: #ffffff;
            padding: 16px;
        }
        .panel-header {
            font-size: 12px;
            font-weight: 700;
            letter-spacing: 0.5px;
            text-transform: uppercase;
            border-bottom: 1px solid #eeeeee;
            padding-bottom: 8px;
            margin-bottom: 14px;
            display: flex;
            justify-content: space-between;
            color: #222222;
        }

        /* CAMERA */
        .camera-container {
            width: 100%;
            aspect-ratio: 4 / 3;
            background-color: #fafafa;
            border: 1px solid #e0e0e0;
            display: flex;
            align-items: center;
            justify-content: center;
            overflow: hidden;
            position: relative;
        }
        .camera-stream {
            width: 100%;
            height: 100%;
            object-fit: contain;
            display: block;
        }
        .camera-placeholder {
            color: #888888;
            font-size: 12px;
            text-align: center;
        }
        .camera-overlay {
            position: absolute;
            top: 6px;
            left: 6px;
            background: #ffffff;
            border: 1px solid #cccccc;
            padding: 2px 6px;
            font-size: 10px;
            font-weight: 600;
        }

        /* SENSORS */
        .sensor-table {
            width: 100%;
            border-collapse: collapse;
        }
        .sensor-table tr {
            border-bottom: 1px solid #f0f0f0;
        }
        .sensor-table tr:last-child {
            border-bottom: none;
        }
        .sensor-table td {
            padding: 8px 0;
        }
        .sensor-label {
            color: #555555;
            font-size: 12px;
        }
        .sensor-value {
            text-align: right;
            font-weight: 600;
            font-size: 13px;
        }
        .tag {
            display: inline-block;
            padding: 2px 6px;
            font-size: 11px;
            font-weight: 600;
            border: 1px solid #000000;
        }
        .tag.active {
            background-color: #000000;
            color: #ffffff;
            border-color: #000000;
        }
        .tag.idle {
            background-color: #ffffff;
            color: #777777;
            border-color: #cccccc;
        }

        /* CONTROLS */
        .controls-grid {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 8px;
        }
        .btn {
            background-color: #ffffff;
            color: #000000;
            border: 1px solid #cccccc;
            padding: 10px 12px;
            font-family: inherit;
            font-size: 11px;
            font-weight: 600;
            cursor: pointer;
            text-align: left;
            display: flex;
            justify-content: space-between;
            align-items: center;
            transition: background 0.15s, border-color 0.15s;
        }
        .btn:hover {
            border-color: #000000;
            background-color: #f7f7f7;
        }
        .btn.active {
            background-color: #000000;
            color: #ffffff;
            border-color: #000000;
        }

        /* RFID */
        .rfid-input-group {
            display: flex;
            gap: 6px;
            margin-bottom: 10px;
        }
        .text-input {
            flex: 1;
            background: #ffffff;
            border: 1px solid #cccccc;
            color: #000000;
            font-family: inherit;
            font-size: 12px;
            padding: 7px 10px;
            text-transform: uppercase;
        }
        .text-input:focus {
            outline: none;
            border-color: #000000;
        }
        .btn-action {
            background: #ffffff;
            color: #000000;
            border: 1px solid #cccccc;
            padding: 7px 12px;
            font-family: inherit;
            font-size: 11px;
            font-weight: 600;
            text-transform: uppercase;
            cursor: pointer;
            transition: all 0.15s;
        }
        .btn-action:hover {
            border-color: #000000;
            background-color: #f7f7f7;
        }
        .btn-action.active {
            background: #000000;
            color: #ffffff;
            border-color: #000000;
        }

        .rfid-list {
            list-style: none;
            max-height: 120px;
            overflow-y: auto;
            border: 1px solid #eeeeee;
        }
        .rfid-item {
            padding: 6px 10px;
            border-bottom: 1px solid #f5f5f5;
            display: flex;
            justify-content: space-between;
            align-items: center;
            font-size: 12px;
        }
        .rfid-item:last-child {
            border-bottom: none;
        }
        .btn-del {
            background: none;
            border: none;
            color: #888888;
            cursor: pointer;
            font-family: inherit;
            font-size: 11px;
            padding: 0 4px;
        }
        .btn-del:hover {
            color: #000000;
            text-decoration: underline;
        }

        /* LOGS */
        .log-box {
            font-size: 11px;
            color: #555555;
            max-height: 150px;
            overflow-y: auto;
            border: 1px solid #eeeeee;
            padding: 8px 10px;
            background: #fafafa;
        }
        .log-line {
            padding: 2px 0;
            border-bottom: 1px solid #f0f0f0;
            white-space: nowrap;
            overflow: hidden;
            text-overflow: ellipsis;
        }
    </style>
</head>
<body>
    <div class="container">
        <header>
            <div>
                <div class="title">ESP32-S3 Camera & Control</div>
            </div>
            <div class="meta">
                <div>Trạng thái: <span id="conn-badge" class="status-badge offline">Đang kết nối...</span></div>
                <div id="last-seen-text" style="margin-top: 2px;">--</div>
            </div>
        </header>

        <div class="grid">
            <!-- CỘT TRÁI: CAMERA STREAM & NHẬT KÝ -->
            <div>
                <div class="panel">
                    <div class="panel-header">
                        <span>Hình ảnh Camera</span>
                        <span id="cam-info">Chờ tín hiệu</span>
                    </div>
                    <div class="camera-container">
                        <img id="stream-img" class="camera-stream" src="/stream" alt="" onerror="handleImgError()" onload="handleImgLoad()">
                        <div id="cam-placeholder" class="camera-placeholder" style="display: none;">Chưa có luồng video</div>
                        <div id="cam-age" class="camera-overlay">Đang nhận luồng</div>
                    </div>
                </div>

                <div class="panel" style="margin-top: 24px;">
                    <div class="panel-header">
                        <span>Nhật ký sự kiện</span>
                        <span id="log-count">0 sự kiện</span>
                    </div>
                    <div id="log-box" class="log-box">
                        <div class="log-line">Khởi động hệ thống...</div>
                    </div>
                </div>
            </div>

            <!-- CỘT PHẢI: CẢM BIẾN & ĐIỀU KHIỂN & RFID -->
            <div>
                <!-- CẢM BIẾN -->
                <div class="panel">
                    <div class="panel-header">
                        <span>Cảm biến</span>
                        <span>Trực tiếp</span>
                    </div>
                    <table class="sensor-table">
                        <tr>
                            <td class="sensor-label">Nhiệt độ</td>
                            <td class="sensor-value"><span id="val-temp">--</span> °C</td>
                        </tr>
                        <tr>
                            <td class="sensor-label">Độ ẩm</td>
                            <td class="sensor-value"><span id="val-hum">--</span> %</td>
                        </tr>
                        <tr>
                            <td class="sensor-label">Chuyển động (PIR)</td>
                            <td class="sensor-value"><span id="val-pir" class="tag idle">Bình thường</span></td>
                        </tr>
                        <tr>
                            <td class="sensor-label">Khí gas (MQ-2)</td>
                            <td class="sensor-value"><span id="val-gas" class="tag idle">An toàn</span></td>
                        </tr>
                        <tr>
                            <td class="sensor-label">Cửa / Mưa</td>
                            <td class="sensor-value"><span id="val-door-sensor" class="tag idle">Đóng</span></td>
                        </tr>
                        <tr>
                            <td class="sensor-label">Thời gian hoạt động</td>
                            <td class="sensor-value"><span id="val-uptime">--</span> s</td>
                        </tr>
                    </table>
                </div>

                <!-- ĐIỀU KHIỂN CÔNG TẮC -->
                <div class="panel" style="margin-top: 24px;">
                    <div class="panel-header">
                        <span>Điều khiển thiết bị</span>
                    </div>
                    <div class="controls-grid">
                        <button id="btn-door" class="btn" onclick="triggerDoor()">
                            <span>Khóa cửa</span>
                            <span id="txt-door">[Đang khóa]</span>
                        </button>
                        <button id="btn-fan" class="btn" onclick="toggleActuator('fan')">
                            <span>Quạt</span>
                            <span id="txt-fan">[Tắt]</span>
                        </button>
                        <button id="btn-lamp" class="btn" onclick="toggleActuator('lamp')">
                            <span>Đèn</span>
                            <span id="txt-lamp">[Tắt]</span>
                        </button>
                        <button id="btn-buzzer" class="btn" onclick="toggleActuator('buzzer')">
                            <span>Còi</span>
                            <span id="txt-buzzer">[Tắt]</span>
                        </button>
                    </div>
                </div>

                <!-- QUẢN LÝ THẺ RFID -->
                <div class="panel" style="margin-top: 24px;">
                    <div class="panel-header">
                        <span>Thẻ RFID</span>
                        <span id="rfid-count">0 thẻ</span>
                    </div>
                    <div class="rfid-input-group">
                        <input id="input-uid" type="text" class="text-input" placeholder="Nhập mã UID thẻ">
                        <button class="btn-action" onclick="addRfidCard()">Thêm</button>
                        <button id="btn-enroll" class="btn-action" onclick="toggleEnroll()">Ghi thẻ [Tắt]</button>
                    </div>
                    <ul id="rfid-list" class="rfid-list">
                        <li class="rfid-item"><span>Đang tải...</span></li>
                    </ul>
                </div>
            </div>
        </div>
    </div>

    <script>
        let currentCommands = {};

        function handleImgError() {
            document.getElementById('stream-img').style.display = 'none';
            document.getElementById('cam-placeholder').style.display = 'block';
            document.getElementById('cam-info').innerText = 'Mất tín hiệu';
        }

        function handleImgLoad() {
            document.getElementById('stream-img').style.display = 'block';
            document.getElementById('cam-placeholder').style.display = 'none';
            document.getElementById('cam-info').innerText = 'Đang nhận tín hiệu';
        }

        async function fetchSystemState() {
            try {
                const res = await fetch('/api/web/state');
                if (!res.ok) throw new Error('Network error');
                const data = await res.json();
                renderUI(data);
            } catch (err) {
                const badge = document.getElementById('conn-badge');
                badge.className = 'status-badge offline';
                badge.innerText = 'Mất kết nối';
            }
        }

        function renderUI(data) {
            const dev = data.device;
            currentCommands = data.commands;

            // Header status
            const badge = document.getElementById('conn-badge');
            if (dev.online) {
                badge.className = 'status-badge online';
                badge.innerText = 'Online';
                document.getElementById('last-seen-text').innerText = 'Đang hoạt động';
            } else {
                badge.className = 'status-badge offline';
                badge.innerText = 'Offline';
                document.getElementById('last-seen-text').innerText = 'Không có phản hồi';
            }

            // Sensors
            document.getElementById('val-temp').innerText = dev.temp > 0 ? dev.temp.toFixed(1) : '--';
            document.getElementById('val-hum').innerText = dev.hum > 0 ? dev.hum.toFixed(1) : '--';
            document.getElementById('val-uptime').innerText = dev.uptime ? dev.uptime : '--';

            updateTag('val-pir', dev.pir, 'Có người', 'Bình thường');
            updateTag('val-gas', dev.gas, 'Báo động', 'An toàn');
            updateTag('val-door-sensor', dev.door_sensor, 'Mở', 'Đóng');

            // Controls
            updateBtn('btn-door', 'txt-door', currentCommands.door, '[Mở 30s]', '[Đang khóa]');
            updateBtn('btn-fan', 'txt-fan', currentCommands.fan, '[Bật]', '[Tắt]');
            updateBtn('btn-lamp', 'txt-lamp', currentCommands.lamp, '[Bật]', '[Tắt]');
            updateBtn('btn-buzzer', 'txt-buzzer', currentCommands.buzzer, '[Bật]', '[Tắt]');
            
            const btnEnroll = document.getElementById('btn-enroll');
            if (currentCommands.rfid_enroll) {
                btnEnroll.innerText = 'Ghi thẻ [Bật]';
                btnEnroll.className = 'btn-action active';
            } else {
                btnEnroll.innerText = 'Ghi thẻ [Tắt]';
                btnEnroll.className = 'btn-action';
            }

            // Camera age
            if (data.has_camera) {
                document.getElementById('cam-age').innerText = 'Độ trễ: ' + data.frame_age + 's';
            } else {
                document.getElementById('cam-age').innerText = 'Chờ khung hình';
            }

            // RFID List
            const listEl = document.getElementById('rfid-list');
            document.getElementById('rfid-count').innerText = data.rfid_cards.length + ' thẻ';
            listEl.innerHTML = data.rfid_cards.map(uid => `
                <li class="rfid-item">
                    <span>${uid}</span>
                    <button class="btn-del" onclick="deleteRfidCard('${uid}')">[Xóa]</button>
                </li>
            `).join('');

            // Logs
            const logBox = document.getElementById('log-box');
            document.getElementById('log-count').innerText = data.logs.length + ' sự kiện';
            logBox.innerHTML = data.logs.map(l => `<div class="log-line">${l}</div>`).join('');
        }

        function updateTag(elId, isActive, textActive, textIdle) {
            const el = document.getElementById(elId);
            if (isActive) {
                el.className = 'tag active';
                el.innerText = textActive;
            } else {
                el.className = 'tag idle';
                el.innerText = textIdle;
            }
        }

        function updateBtn(btnId, txtId, state, onText, offText) {
            const btn = document.getElementById(btnId);
            const txt = document.getElementById(txtId);
            if (state) {
                btn.className = 'btn active';
                txt.innerText = onText;
            } else {
                btn.className = 'btn';
                txt.innerText = offText;
            }
        }

        async function postCommand(payload) {
            await fetch('/api/web/command', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify(payload)
            });
            fetchSystemState();
        }

        function toggleActuator(name) {
            const current = currentCommands[name] || 0;
            postCommand({ [name]: current ? 0 : 1 });
        }

        function triggerDoor() {
            postCommand({ door: 1 });
        }

        function toggleEnroll() {
            const current = currentCommands.rfid_enroll || 0;
            postCommand({ rfid_enroll: current ? 0 : 1 });
        }

        async function addRfidCard() {
            const input = document.getElementById('input-uid');
            const uid = input.value.trim().toUpperCase();
            if (!uid) return;
            const res = await fetch('/api/web/rfid/add', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({ uid: uid })
            });
            input.value = '';
            fetchSystemState();
        }

        async function deleteRfidCard(uid) {
            if (!confirm('Xóa mã thẻ ' + uid + '?')) return;
            await fetch('/api/web/rfid/delete', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({ uid: uid })
            });
            fetchSystemState();
        }

        setInterval(fetchSystemState, 1500);
        fetchSystemState();
    </script>
</body>
</html>
"""

@app.route('/')
def dashboard():
    return render_template_string(HTML_DASHBOARD)

@app.route('/favicon.ico')
def favicon():
    return send_from_directory(
        os.path.dirname(os.path.abspath(__file__)),
        'icon.png',
        mimetype='image/png'
    )

if __name__ == '__main__':
    port = int(os.environ.get("PORT", 5555))
    app.run(host='0.0.0.0', port=port, threaded=True)
