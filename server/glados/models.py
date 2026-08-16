import json
import subprocess
import time
import urllib.request

import gigaam
import webrtcvad
from openai import OpenAI
from ruaccent import RUAccent
from TeraTTS import TTS

from . import config

try:
    from ddgs import DDGS
    internet_search_available = True
except ImportError:
    try:
        from duckduckgo_search import DDGS
        internet_search_available = True
    except ImportError:
        DDGS = None
        internet_search_available = False
        print("[internet] ddgs not installed (pip install ddgs) - web search disabled")

def _lm_studio_port():
    return config.lm_studio_url.rstrip("/").rsplit(":", 1)[-1].split("/")[0]

def _lm_studio_ready():
    try:
        with urllib.request.urlopen(f"{config.lm_studio_url}/models", timeout=2) as resp:
            data = json.loads(resp.read())
            return bool(data.get("data"))
    except Exception:
        return False

def launch_lm_studio_background():
    if not config.lms_auto_start or _lm_studio_ready():
        return
    print("[lm studio] not ready yet, launching 'lms server start' in the background...")
    try:
        subprocess.Popen(
            ["lms", "server", "start", "--port", _lm_studio_port()],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
    except FileNotFoundError:
        print(
            "[lm studio] 'lms' CLI not found on PATH. One-time setup: LM Studio -> "
            "Developer tab -> 'Install CLI' (or run `lms bootstrap` in LM Studio's "
            "built-in terminal), then restart this script."
        )
    except Exception as e:
        print(f"[lm studio] failed to launch 'lms server start': {e!r}")

def ensure_lm_studio_ready():
    if not config.lms_auto_start or _lm_studio_ready():
        return
    deadline = time.time() + config.lms_startup_timeout_s
    while time.time() < deadline:
        if _lm_studio_ready():
            print("[lm studio] server is up with a model loaded")
            return
        time.sleep(config.lms_poll_interval_s)

    print(f"[lm studio] still not ready, trying 'lms load {config.lm_studio_model}' explicitly...")
    try:
        subprocess.run(["lms", "load", config.lm_studio_model], timeout=config.lms_startup_timeout_s)
    except FileNotFoundError:
        print("[lm studio] 'lms' CLI not found - see setup note above")
        return
    except Exception as e:
        print(f"[lm studio] 'lms load {config.lm_studio_model}' failed: {e!r}")
        return

    if _lm_studio_ready():
        print("[lm studio] model loaded")
    else:
        print(
            f"[lm studio] still couldn't confirm readiness. Check the model key with "
            f"`lms ls` - lm_studio_model (currently {config.lm_studio_model!r}) has to match "
            f"exactly, or load it manually once in the LM Studio UI."
        )

launch_lm_studio_background()

print(f"loading gigaam ({config.gigaam_model_version})...")
asr_model = gigaam.load_model(config.gigaam_model_version)
try:
    asr_model = asr_model.to(config.gigaam_device)
except Exception as e:
    print(f"[gigaam] couldn't move model to {config.gigaam_device!r}, staying on default device: {e!r}")

print("loading accentizer...")
accentizer = RUAccent()
_accentizer_custom_dict = {
    'ГЛаДОС': 'ГЛ+А+ДОС',
    'ГЛАДОС': 'ГЛ+АДОС',
    'ГлаДОС': 'Гл+аДОС',
    'ИИ': '+И-+И',
    'АИ': '+А-+И',
}
accentizer.load(omograph_model_size='turbo', use_dictionary=True, custom_dict=_accentizer_custom_dict, device="CPU")

print("loading tts...")
tts = TTS("TeraTTS/glados2-g2p-vits", add_time_to_end=0.3, tokenizer_load_dict=True, device="CPU")

llm_client = OpenAI(base_url=config.lm_studio_url, api_key="lm-studio")

ensure_lm_studio_ready()

vad = webrtcvad.Vad(config.vad_aggressiveness)

print("all models loaded, starting server")
