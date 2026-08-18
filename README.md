# PotatOS / GLaDOS

ESP32 box + mic + speaker. Say "Гладос" + question, she answers in GLaDOS's voice.

---

## Usee

1. Start `start.bat`
2. Wait 1-3 min
3. Green "ALL READY" = good. Red "PROBLEM DETECTED" = check which step failed, see Troubleshooting.

Close the launcher after. Keep "GLaDOS Server - LIVE LOG" window open if you want to see what she's hearing/saying.

**Talking:** "Гладос" + question. 10 sec window to keep talking after her reply without repeating the wake word. She pulls live web info herself for news/weather/date, no need to ask.

**Button:**
- tap = mute/unmute mic
- hold 5s = open Wi-Fi setup menu (change module/volume)
- hold 15s = full Wi-Fi reset

**Modules** - say "Гладос, модуль <name>":
стандарт, уитли, космос, факт, приключений, любопытства, морали

---

## Install (new PC)

**1. Python + packages**

```bat
pip install websockets python-dotenv numpy scipy soundfile webrtcvad ^
    openai ruaccent TeraTTS huggingface_hub ddgs
```

**2. GigaAM (speech-to-text)**

```bat
git clone https://github.com/salute-developers/GigaAM.git
cd GigaAM
pip install -e .[torch]
```

If `onnxruntime==1.23.*` fails to install: edit `GigaAM\pyproject.toml`, change to `onnx>=1.19` and `onnxruntime>=1.23`, rerun install. Model files (~1-2GB) auto-download on first server start.

**3. LM Studio**

Install from lmstudio.ai. Load model `meta-llama-3-8b-instruct` (or whatever `lm_studio_model` is set to in `glados/config.py`). Developer tab → Local Server → "Serve on Local Network" on, port 1234.

**4. ngrok**

Install from ngrok.com.

```bat
ngrok config add-authtoken YOUR_TOKEN
```

Reserved domain must already exist on your ngrok account.

**5. Edit `start.bat`** — check these paths match your machine:

```bat
set "PYTHON_EXE=C:\Users\Ardor\AppData\Local\Python\bin\python.exe"
set "SERVER_SCRIPT=G:\projects\PotatOS\main\server\server.py"
set "LMSTUDIO_EXE=C:\Users\Ardor\AppData\Local\Programs\LM Studio\LM Studio.exe"
set "NGROK_DOMAIN=amari-formic-helene.ngrok-free.dev"
```

---

## Flashing the ESP32 (once per board)

Firmware: `main.cpp` (PlatformIO/Arduino). Libraries: `WiFiManager`, `WebSocketsClient`, `Preferences`, built-in I2S driver.

In `main.cpp`, match your ngrok domain:

```cpp
const char* ws_host = "amari-formic-helene.ngrok-free.dev";
const uint16_t ws_port = 443;
const bool ws_use_ssl = true;
```

Flash over USB as usual.

**First boot:** board opens hotspot `Potato-GLaDOS-Setup` / password `potato1234`. Connect, go to `192.168.4.1`, pick your home Wi-Fi.

---

## Troubleshooting

- **Red LED, not blinking** - can't reach server. Confirm `start.bat` said "ALL READY." Just rebooted? Wait a few sec.
- **LM Studio fails** - open it manually, Developer tab, model loaded + "Serve on Local Network" on.
- **ngrok fails** - check `logs\ngrok_*.log`. Usually missing authtoken or domain not on your account.
- **Server crashes on startup** - check "LIVE LOG" window for the error.
- **Bad recognition** - get closer to mic, speak clearer, check for electrical noise nearby.
- **ESP32 disconnects mid-reply** - check serial log for reset reason + RSSI. Server auto-resends on reconnect.
