import re

import numpy as np
from transliterate import translit

from . import audio
from . import config
from . import models

def _tts_infer_raw(tts_instance, text, noise_scale, length_scale, noise_scale_w):
    if tts_instance.preprocess_trans:
        text = translit(text, 'ru')
    if tts_instance.preprocess_nums:
        text = re.sub(r'\d+', tts_instance._num2wordsshor, text)

    phoneme_ids = tts_instance._get_seq(text)
    text_arr = np.expand_dims(np.array(phoneme_ids, dtype=np.int64), 0)
    text_lengths = np.array([text_arr.shape[1]], dtype=np.int64)
    scales = np.array([noise_scale, length_scale, noise_scale_w], dtype=np.float32)

    audio = tts_instance.model.run(
        None,
        {
            "input": text_arr,
            "input_lengths": text_lengths,
            "scales": scales,
            "sid": None,
        },
    )[0][0, 0][0]
    return tts_instance._add_silent(audio, silence_duration=tts_instance.add_time_to_end)

def synthesize(text, module=None):
    if module is None:
        module = config.default_glados_module

    processed = models.accentizer.process_all(text)
    voice = config.glados_module_voice_scales.get(module, config.glados_module_voice_scales[config.default_glados_module])
    audio_out = _tts_infer_raw(
        models.tts, processed,
        noise_scale=voice["noise_scale"],
        length_scale=voice["length_scale"],
        noise_scale_w=voice["noise_scale_w"],
    )

    if config.potato_mode:
        audio_out = audio.apply_potato_effect(audio_out, config.tts_sample_rate)

    return audio_out
