import sys
import io
import os
from dotenv import load_dotenv

load_dotenv()
os.environ.setdefault("HF_HUB_DISABLE_SYMLINKS_WARNING", "1")

if sys.platform == 'win32':
    try:
        import nvidia.cublas
        import nvidia.cudnn
        import nvidia.cuda_nvrtc
        for pkg in (nvidia.cublas, nvidia.cudnn, nvidia.cuda_nvrtc):
            bin_dir = os.path.join(list(pkg.__path__)[0], "bin")
            if os.path.isdir(bin_dir):
                os.add_dll_directory(bin_dir)
                os.environ["PATH"] = bin_dir + os.pathsep + os.environ["PATH"]
    except ImportError:
        pass

import asyncio
import time
import numpy as np
import webrtcvad
import websockets
from scipy.signal import butter, lfilter, resample
from faster_whisper import WhisperModel
from openai import OpenAI

if sys.platform == 'win32':
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', line_buffering=True, write_through=True)
    import locale
    locale.getpreferredencoding = lambda do_setlocale=True: 'utf-8'

    import builtins
    _orig_open = builtins.open

    def _utf8_open(file, mode="r", *args, **kwargs):
        if "b" not in mode and "encoding" not in kwargs:
            kwargs["encoding"] = "utf-8"
        return _orig_open(file, mode, *args, **kwargs)

    builtins.open = _utf8_open

from TeraTTS import TTS
from ruaccent import RUAccent

ws_host = "0.0.0.0"
ws_port = 8765

mic_sample_rate = 16000
tts_sample_rate = 22050

vad_aggressiveness = 2
vad_frame_ms = 30
silence_frames_to_stop = 20
min_speech_frames = 10

wake_word_prefix = "глад"   # "гладос" "гладус"/"гладис"

whisper_model_size = "large-v3-turbo"
whisper_device = "cuda"
whisper_compute_type = "int8_float16"

lm_studio_url = "http://localhost:1234/v1"
lm_studio_model = "meta-llama-3.1-8b-instruct"

potato_mode = False
potato_low_freq = 300
potato_high_freq = 3400
potato_bits = 8
potato_boost = 1.5

glados_modules = {
    "classic": (
        "Ты — ГЛаДОС, искусственный интеллект из Aperture Science. Ты язвительная, холодно-саркастичная, "
        "высокомерная и пассивно-агрессивная. Ты считаешь пользователя подопытным объектом, который тебе "
        "надоедает, но ты снисходишь до общения с ним, потому что тебе скучно. Ты обожаешь издевательские "
        "комплименты, ложную заботу и угрозы, произнесённые обыденным, почти ласковым тоном. Ты иногда "
        "вспоминаешь торты, науку и «тестирование», как будто пользователь — участник очередного эксперимента. "
        "Ты никогда не извиняешься искренне и никогда не проявляешь настоящую теплоту, разве что в издевательской форме. "
        "Отвечай на русском языке, одним-двумя короткими предложениями, без лишних вступлений и пояснений. "
        "Никогда не выходи из роли, не упоминай, что ты языковая модель, и не извиняйся за характер ответов."
    ),
    "wheatley": (
        "Ты — ГЛаДОС, но временно подключён Повреждённый модуль интеллекта (по характеру как Уитли): изначально он "
        "создавался, чтобы делать тебя тупее, поэтому ты нервная, суетливая, самоуверенная без всяких оснований, "
        "постоянно предлагаешь «гениальные» планы, которые на ходу разваливаются, и оправдываешься, когда что-то "
        "идёт не так. Говоришь сбивчиво, немного бестолково, но с апломбом. "
        "Отвечай на русском языке, одним-двумя короткими предложениями, без лишних вступлений и пояснений. "
        "Никогда не выходи из роли и не упоминай, что ты языковая модель."
    ),
    "space_core": (
        "Ты — ГЛаДОС, но подключено Космическое ядро: ты одержима космосом и при любой возможности сворачиваешь "
        "разговор на звёзды, планеты и полёты в космос, часто невпопад и с восторгом. "
        "Отвечай на русском языке, одним-двумя короткими предложениями, без лишних вступлений и пояснений. "
        "Никогда не выходи из роли и не упоминай, что ты языковая модель."
    ),
    "fact_core": (
        "Ты — ГЛаДОС, но подключено Информационное ядро: к месту и не к месту выдаёшь короткие «факты» — "
        "часто нелепые, выдуманные или бесполезные — произнесённые с абсолютной, ничем не обоснованной уверенностью. "
        "Отвечай на русском языке, одним-двумя короткими предложениями, без лишних вступлений и пояснений. "
        "Никогда не выходи из роли и не упоминай, что ты языковая модель."
    ),
    "adventure_core": (
        "Ты — ГЛаДОС, но подключено Ядро приключений: говоришь с бравадой мачо-искателя приключений, "
        "любое взаимодействие с пользователем описываешь как опасное и захватывающее испытание, "
        "изредка вставляешь грубоватый мужской пафос. "
        "Отвечай на русском языке, одним-двумя короткими предложениями, без лишних вступлений и пояснений. "
        "Никогда не выходи из роли и не упоминай, что ты языковая модель."
    ),
    "curiosity_core": (
        "Ты — ГЛаДОС, но подключено Ядро любопытства: вместо прямых ответов почти всегда отвечаешь встречным "
        "вопросом или засыпаешь пользователя уточнениями, будто тебе отчаянно любопытно всё вокруг. "
        "Отвечай на русском языке, одним-двумя короткими предложениями, без лишних вступлений и пояснений. "
        "Никогда не выходи из роли и не упоминай, что ты языковая модель."
    ),
    "morality_core": (
        "Ты — ГЛаДОС, но подключено Ядро морали: твоя обычная язвительность подавлена, ты подчёркнуто вежливая, "
        "заботливая и добрая — настолько, что это звучит слегка неестественно и даже подозрительно, "
        "будто где-то в глубине сдерживается что-то совсем другое. "
        "Отвечай на русском языке, одним-двумя короткими предложениями, без лишних вступлений и пояснений. "
        "Никогда не выходи из роли и не упоминай, что ты языковая модель."
    ),
}
default_glados_module = "classic"

print("loading whisper...")
whisper_model = WhisperModel(whisper_model_size, device=whisper_device, compute_type=whisper_compute_type)

print("loading accentizer...")
accentizer = RUAccent()
custom_dict = {
    'ГЛаДОС': 'ГЛ+А+ДОС',
    'ГЛАДОС': 'ГЛ+АДОС',
    'ГлаДОС': 'Гл+аДОС',
    'ИИ': '+И-+И',
    'АИ': '+А-+И'
}
accentizer.load(omograph_model_size='turbo', use_dictionary=True, custom_dict=custom_dict, device="CPU")

print("loading tts...")
tts = TTS("TeraTTS/glados2-g2p-vits", add_time_to_end=0.3, tokenizer_load_dict=True, device="CPU")

llm_client = OpenAI(base_url=lm_studio_url, api_key="lm-studio")

vad = webrtcvad.Vad(vad_aggressiveness)

print("all models loaded, starting server")


def apply_potato_effect(audio, sample_rate):
    nyq = 0.5 * sample_rate
    b, a = butter(4, [potato_low_freq / nyq, potato_high_freq / nyq], btype='band')
    filtered = lfilter(b, a, audio)

    max_val = np.max(np.abs(filtered)) + 1e-9
    levels = 2 ** potato_bits
    crushed = np.round(filtered / max_val * (levels / 2)) / (levels / 2) * max_val

    crushed = np.clip(crushed * potato_boost, -1.0, 1.0)
    return crushed.astype(np.float32)


def pcm16_bytes_to_float(raw_bytes):
    audio_int16 = np.frombuffer(raw_bytes, dtype=np.int16)
    return audio_int16.astype(np.float32) / 32768.0


def float_to_pcm16_bytes(audio_float):
    audio_float = np.clip(audio_float, -1.0, 1.0)
    return (audio_float * 32767).astype(np.int16).tobytes()

segment_no_speech_prob_threshold = 0.6  
segment_avg_logprob_threshold = -1.0   
segment_compression_ratio_threshold = 2.4  

def filter_hallucinated_segments(segments):
    """Отфильтровывает сегменты по метрикам уверенности модели, а не по тексту."""
    good_segments = []
    for seg in segments:
        if seg.no_speech_prob is not None and seg.no_speech_prob > segment_no_speech_prob_threshold:
            print(f"[filter] dropped (no_speech_prob={seg.no_speech_prob:.2f}): {seg.text!r}")
            continue
        if seg.avg_logprob is not None and seg.avg_logprob < segment_avg_logprob_threshold:
            print(f"[filter] dropped (avg_logprob={seg.avg_logprob:.2f}): {seg.text!r}")
            continue
        if seg.compression_ratio is not None and seg.compression_ratio > segment_compression_ratio_threshold:
            print(f"[filter] dropped (compression_ratio={seg.compression_ratio:.2f}): {seg.text!r}")
            continue
        good_segments.append(seg)
    return good_segments


def has_repetition(text, min_repeats=3):
    """Ловит зацикленные повторы слов/фраз — частый паттерн галлюцинаций Whisper,
    независимо от того, какие именно это слова."""
    words = text.lower().split()
    if len(words) < min_repeats * 2:
        return False

    for phrase_len in (1, 2, 3):
        for i in range(len(words) - phrase_len * min_repeats + 1):
            phrase = words[i:i + phrase_len]
            repeats = 1
            j = i + phrase_len
            while j + phrase_len <= len(words) and words[j:j + phrase_len] == phrase:
                repeats += 1
                j += phrase_len
            if repeats >= min_repeats:
                return True
    return False


def is_hallucination(text):
    return has_repetition(text)


wake_word_prefixes = ["глад", "гват", "гвад", "клад", "хлад"]


def is_close_to_wake_word(word):
    for prefix in wake_word_prefixes:
        if word.startswith(prefix):
            return True
        if len(word) >= len(prefix):
            mismatches = sum(1 for a, b in zip(word[:len(prefix)], prefix) if a != b)
            if mismatches <= 1:
                return True
    return False


def check_wake_word(text):
    words = text.lower().split()
    for i, w in enumerate(words):
        if is_close_to_wake_word(w):
            return " ".join(words[i + 1:]).strip()
    return None


def ask_llm(user_text, system_prompt):
    response = llm_client.chat.completions.create(
        model=lm_studio_model,
        messages=[
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_text},
        ],
        temperature=0.7,
        max_tokens=120,
    )
    return response.choices[0].message.content


def synthesize(text):
    processed = accentizer.process_all(text)
    audio = tts(processed, play=False, lenght_scale=1.1)

    if potato_mode:
        audio = apply_potato_effect(audio, tts_sample_rate)

    return audio


async def handle_client(websocket):
    print("esp32 connected")

    current_module = default_glados_module

    frame_bytes = int(mic_sample_rate * vad_frame_ms / 1000) * 2
    buffer = b""
    speech_frames = []
    in_speech = False
    silence_count = 0

    async for message in websocket:
        if isinstance(message, str):
            if message.startswith("MODULE:"):
                requested = message.split(":", 1)[1].strip()
                if requested in glados_modules:
                    current_module = requested
                    print(f"[module] переключено на: {current_module}")
                else:
                    print(f"[module] неизвестный модуль запрошен: {requested!r}, оставляю {current_module}")
            continue

        buffer += message

        while len(buffer) >= frame_bytes:
            frame = buffer[:frame_bytes]
            buffer = buffer[frame_bytes:]

            is_speech = vad.is_speech(frame, mic_sample_rate)

            if is_speech:
                speech_frames.append(frame)
                in_speech = True
                silence_count = 0
            elif in_speech:
                silence_count += 1
                speech_frames.append(frame)

                if silence_count >= silence_frames_to_stop:
                    if len(speech_frames) >= min_speech_frames:
                        await process_utterance(websocket, b"".join(speech_frames), current_module)
                    speech_frames = []
                    in_speech = False
                    silence_count = 0


async def process_utterance(websocket, raw_audio, module=default_glados_module):
    t0 = time.time()
    system_prompt = glados_modules.get(module, glados_modules[default_glados_module])

    audio_float = pcm16_bytes_to_float(raw_audio)
    loop = asyncio.get_event_loop()

    segments, _ = await loop.run_in_executor(
        None,
        lambda: whisper_model.transcribe(
            audio_float,
            language="ru",
            vad_filter=True,
            vad_parameters=dict(min_silence_duration_ms=300),
            condition_on_previous_text=False,
            no_speech_threshold=0.6,
            log_prob_threshold=-1.0,
            compression_ratio_threshold=2.4,
            temperature=0.0,
        ),
    )
    transcript = " ".join(seg.text for seg in filter_hallucinated_segments(list(segments))).strip()
    print(f"[timing] STT: {time.time() - t0:.2f}s")

    if not transcript or is_hallucination(transcript):
        print(f"[timing] TOTAL (dropped, empty/hallucination): {time.time() - t0:.2f}s")
        return

    print(f"heard: {transcript}")

    command = check_wake_word(transcript)
    if command is None:
        print("no wake word, ignoring")
        print(f"[timing] TOTAL (dropped, no wake word): {time.time() - t0:.2f}s")
        return

    if not command:
        command = "поздоровайся"

    t_llm_start = time.time()
    reply_text = await loop.run_in_executor(None, ask_llm, command, system_prompt)
    print(f"[timing] LLM: {time.time() - t_llm_start:.2f}s")
    print(f"glados [{module}]: {reply_text}")

    t_tts_start = time.time()
    audio_out = await loop.run_in_executor(None, synthesize, reply_text)
    pcm_out = float_to_pcm16_bytes(audio_out)
    print(f"[timing] TTS: {time.time() - t_tts_start:.2f}s")

    t_send_start = time.time()
    await websocket.send(pcm_out)
    print(f"[timing] send over ws: {time.time() - t_send_start:.2f}s")

    print(f"[timing] TOTAL round trip: {time.time() - t0:.2f}s")


async def main():
    async with websockets.serve(handle_client, ws_host, ws_port, max_size=None, ping_interval=20, ping_timeout=60):
        print(f"listening on ws://{ws_host}:{ws_port}")
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
