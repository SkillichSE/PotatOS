import asyncio

import numpy as np
import soundfile as sf
from scipy.signal import butter, lfilter, resample

from . import config

def apply_potato_effect(audio, sample_rate):
    nyq = 0.5 * sample_rate
    b, a = butter(4, [config.potato_low_freq / nyq, config.potato_high_freq / nyq], btype='band')
    filtered = lfilter(b, a, audio)

    max_val = np.max(np.abs(filtered)) + 1e-9
    levels = 2 ** config.potato_bits
    crushed = np.round(filtered / max_val * (levels / 2)) / (levels / 2) * max_val

    crushed = np.clip(crushed * config.potato_boost, -1.0, 1.0)
    return crushed.astype(np.float32)

def save_wav_for_asr(path, audio_float, sample_rate):
    sf.write(path, audio_float, sample_rate, subtype="PCM_16")

def pcm16_bytes_to_float(raw_bytes):
    audio_int16 = np.frombuffer(raw_bytes, dtype=np.int16)
    return audio_int16.astype(np.float32) / 32768.0

def float_to_pcm16_bytes(audio_float):
    audio_float = np.clip(audio_float, -1.0, 1.0)
    return (audio_float * 32767).astype(np.int16).tobytes()

def float_to_pcm8_bytes(audio_float):
    audio_float = np.clip(audio_float, -1.0, 1.0)
    silence_gate = 0.012
    audio_float = np.where(np.abs(audio_float) < silence_gate, 0.0, audio_float)
    audio_u8 = np.round((audio_float * 127.5) + 127.5)
    return np.clip(audio_u8, 0, 255).astype(np.uint8).tobytes()

def load_mp3_as_pcm8(path):
    audio, sr = sf.read(path, dtype="float32", always_2d=False)
    if audio.ndim > 1:
        audio = audio.mean(axis=1)
    if sr != config.tts_sample_rate:
        target_len = int(round(len(audio) * config.tts_sample_rate / sr))
        audio = resample(audio, target_len)
    return float_to_pcm8_bytes(audio.astype(np.float32))

async def send_audio_chunked(websocket, pcm_bytes, chunk_size=config.AUDIO_CHUNK_BYTES, paced=True,
                              progress=None, base_offset=0):
    if not paced:
        for offset in range(0, len(pcm_bytes), chunk_size):
            chunk = pcm_bytes[offset:offset + chunk_size]
            await websocket.send(chunk)
            if progress is not None:
                progress["sent_bytes"] = base_offset + offset + len(chunk)
        return

    loop = asyncio.get_event_loop()
    start = loop.time()
    sent_duration_s = 0.0
    for offset in range(0, len(pcm_bytes), chunk_size):
        chunk = pcm_bytes[offset:offset + chunk_size]
        target_time = start + sent_duration_s - config.send_pace_lead_s
        now = loop.time()
        if target_time > now:
            await asyncio.sleep(target_time - now)
        await websocket.send(chunk)
        if progress is not None:
            progress["sent_bytes"] = base_offset + offset + len(chunk)
        sent_duration_s += len(chunk) / config.tts_wire_bytes_per_second
