import sys
import io
import time
import numpy as np
from scipy.signal import butter, lfilter
import sounddevice as sd

# config
potato_mode = True
low_freq = 300
high_freq = 3400
bits = 8
boost = 1.5

if sys.platform == 'win32':
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
    import builtins
    _original_open = builtins.open
    def patched_open(file, mode='r', *args, **kwargs):
        if 'encoding' not in kwargs and 'b' not in mode:
            kwargs['encoding'] = 'utf-8'
        return _original_open(file, mode, *args, **kwargs)
    builtins.open = patched_open

from TeraTTS import TTS
from ruaccent import RUAccent


def apply_potato_effect(audio, sample_rate=22050):
    nyq = 0.5 * sample_rate
    b, a = butter(4, [low_freq / nyq, high_freq / nyq], btype='band')
    filtered = lfilter(b, a, audio)

    max_val = np.max(np.abs(filtered)) + 1e-9
    levels = 2 ** bits
    crushed = np.round(filtered / max_val * (levels / 2)) / (levels / 2) * max_val

    crushed = np.clip(crushed * boost, -1.0, 1.0)
    return crushed.astype(np.float32)


accentizer = RUAccent()
custom_dict = {
    'ГЛаДОС': 'ГЛ+А+ДОС',
    'ГЛАДОС': 'ГЛ+АДОС',
    'ГлаДОС': 'Гл+аДОС',
    'ИИ': '+И-+И',
    'АИ': '+А-+И'
}
accentizer.load(omograph_model_size='turbo', use_dictionary=True, custom_dict=custom_dict, device="CPU")

tts = TTS("TeraTTS/glados2-g2p-vits", add_time_to_end=1.0, tokenizer_load_dict=True, device="CPU")

while True:
    text = input("text: ").strip()
    if not text:
        break

    t0 = time.time()
    processed = accentizer.process_all(text)
    audio = tts(processed, play=False, lenght_scale=1.1)

    if potato_mode:
        audio = apply_potato_effect(audio)

    sd.play(audio, samplerate=22050, blocking=True)
    print(f"took {time.time() - t0:.2f}s")