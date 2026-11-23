from flask import Flask, request, jsonify, render_template_string, Response
import time

app = Flask(__name__)

# Bộ nhớ tạm lưu trạng thái thiết bị và lệnh
device_state = {
    "pir": False,
    "gas": False,
    "door_sensor": False,
    "temp": 0.0,
    "hum": 0.0,
    "uptime": 0,
    "last_seen": 0
}

device_commands = {
    "fan": 0,
    "lamp": 0,
    "door": 0,
    "buzzer": 0,
    "rfid_enroll": 0
}

latest_jpeg_frame = None

# 1. ESP32 gửi dữ liệu cảm biến
@app.route('/api/device/status', methods=['POST'])
def receive_status():
    global device_state
    data = request.get_json(silent=True) or {}
    device_state.update(data)
    device_state["last_seen"] = int(time.time())
    return jsonify({"status": "OK"})

# 2. ESP32 lấy lệnh điều khiển (long-polling hoặc chu kỳ)
@app.route('/api/device/command', methods=['GET'])
def get_commands():
    return jsonify(device_commands)

# 3. ESP32 đẩy ảnh camera JPEG lên server
@app.route('/api/device/camera/upload', methods=['POST'])
def upload_camera_frame():
    global latest_jpeg_frame
    latest_jpeg_frame = request.data
    return jsonify({"status": "OK", "bytes": len(latest_jpeg_frame)})

# 4. API kiểm tra thẻ RFID
@app.route('/api/device/rfid/check', methods=['POST'])
def check_rfid():
    data = request.get_json(silent=True) or {}
    uid = data.get("uid", "")
    # Danh sách UID hợp lệ (bạn có thể lưu vào database/file)
    AUTHORIZED_UIDS = ["A1 B2 C3 D4", "12 34 56 78"]
    is_valid = uid in AUTHORIZED_UIDS
    return jsonify({"authorized": is_valid})

# 5. Luồng Video Stream cho trình duyệt (Flask phục vụ, ESP32 không bị tốn tài nguyên)
def gen_frames():
    while True:
        if latest_jpeg_frame:
            yield (b'--frame\r\n'
                   b'Content-Type: image/jpeg\r\n\r\n' + latest_jpeg_frame + b'\r\n')
        time.sleep(0.1)

@app.route('/stream')
def video_stream():
    return Response(gen_frames(), mimetype='multipart/x-mixed-replace; boundary=frame')

# 6. Web Dashboard hiện đại
@app.route('/')
def dashboard():
    return render_template_string('''
    <!DOCTYPE html>
    <html>
    <head>
        <meta charset="utf-8"><title>Smart Home Cloud Dashboard</title>
        <meta name="viewport" content="width=device-width, initial-scale=1">
        <script src="https://cdn.tailwindcss.com"></script>
    </head>
    <body class="bg-slate-900 text-white min-h-screen p-4 flex flex-col items-center">
        <h1 class="text-2xl font-bold mb-4 text-sky-400">🏠 Smart Home Cloud Controller</h1>
        <div class="grid grid-cols-1 md:grid-cols-2 gap-4 max-w-4xl w-full">
            <!-- Camera View -->
            <div class="bg-slate-800 p-4 rounded-xl shadow">
                <h2 class="text-lg font-semibold mb-2">📷 Live Camera</h2>
                <img src="/stream" class="w-full rounded-lg bg-black border border-slate-700" alt="Camera stream" />
            </div>
            
            <!-- Controls & Sensors -->
            <div class="bg-slate-800 p-4 rounded-xl shadow flex flex-col gap-4">
                <h2 class="text-lg font-semibold">📊 Cảm Biến</h2>
                <div class="grid grid-cols-2 gap-2 text-sm">
                    <div class="bg-slate-700 p-3 rounded">Nhiệt độ: <span id="temp" class="font-bold text-sky-400">--</span>°C</div>
                    <div class="bg-slate-700 p-3 rounded">Độ ẩm: <span id="hum" class="font-bold text-sky-400">--</span>%</div>
                    <div class="bg-slate-700 p-3 rounded">PIR: <span id="pir" class="font-bold">--</span></div>
                    <div class="bg-slate-700 p-3 rounded">Khí Gas: <span id="gas" class="font-bold">--</span></div>
                </div>

                <h2 class="text-lg font-semibold mt-2">⚡ Điều Khiển</h2>
                <div class="grid grid-cols-2 gap-2">
                    <button onclick="toggleCmd('door', 1)" class="bg-blue-600 hover:bg-blue-500 py-2 rounded font-semibold">🚪 Mở Cửa (30s)</button>
                    <button onclick="toggleCmd('fan')" id="btn-fan" class="bg-slate-600 py-2 rounded font-semibold">🌀 Quạt</button>
                    <button onclick="toggleCmd('lamp')" id="btn-lamp" class="bg-slate-600 py-2 rounded font-semibold">💡 Đèn</button>
                    <button onclick="toggleCmd('buzzer')" id="btn-buzzer" class="bg-slate-600 py-2 rounded font-semibold">🔔 Còi</button>
                </div>
            </div>
        </div>

        <script>
            let cmds = {};
            async function fetchState() {
                const res = await fetch('/api/device/command');
                cmds = await res.json();
                // Tự động fetch trạng thái và cập nhật màu nút...
            }
            async function toggleCmd(k, val=null) {
                const newVal = (val !== null) ? val : (cmds[k] ? 0 : 1);
                await fetch('/api/web/command', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify({[k]: newVal})
                });
                fetchState();
            }
            setInterval(fetchState, 2000);
        </script>
    </body>
    </html>
    ''')

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5555)