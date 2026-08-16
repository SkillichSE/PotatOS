import asyncio
import os
import tempfile
import time
import traceback
from datetime import datetime

import websockets

from . import audio
from . import config
from . import llm
from . import models
from . import stt
from . import tts

def _ts():
    return datetime.now().strftime("%H:%M:%S.%f")[:-3]

pending_reply = {"pcm": None, "ready_at": 0.0, "delivered": True, "sent_bytes": 0}

active_websocket = None
current_send_task = None

async def _tracked_send(websocket, pcm_bytes, progress=None, base_offset=0):
    global current_send_task
    task = asyncio.ensure_future(
        audio.send_audio_chunked(websocket, pcm_bytes, progress=progress, base_offset=base_offset)
    )
    current_send_task = task
    try:
        await task
    finally:
        if current_send_task is task:
            current_send_task = None

async def stop_playback():
    global current_send_task
    task = current_send_task
    if task is not None and not task.done():
        task.cancel()
        try:
            await task
        except (asyncio.CancelledError, Exception):
            pass
    pending_reply["delivered"] = True
    ws = active_websocket
    if ws is not None:
        try:
            await ws.send("STOP")
        except Exception:
            pass
        try:
            await ws.send("UNMUTE")
        except Exception:
            pass

def load_persisted_module():
    try:
        saved = config.MODULE_STATE_FILE.read_text(encoding="utf-8").strip()
        if saved in config.glados_modules:
            return saved
        if saved:
            print(f"[module] ignoring unknown module {saved!r} in {config.MODULE_STATE_FILE.name}")
    except FileNotFoundError:
        pass
    except Exception:
        print(f"[module] failed to read {config.MODULE_STATE_FILE.name}, defaulting to {config.default_glados_module}:")
        traceback.print_exc()
    return config.default_glados_module

def persist_module(module_id):
    try:
        config.MODULE_STATE_FILE.write_text(module_id, encoding="utf-8")
    except Exception:
        print(f"[module] failed to persist {module_id!r} to {config.MODULE_STATE_FILE.name}:")
        traceback.print_exc()

current_module = load_persisted_module()

async def switch_module_by_voice(websocket, module_id):
    global current_module
    current_module = module_id
    persist_module(current_module)

    boot_text = config.glados_boot_phrases.get(current_module, config.glados_boot_phrases[config.default_glados_module])
    print(f"[module] voice switch to: {current_module}")
    print(f"[boot] glados [{current_module}]: {boot_text}")

    loop = asyncio.get_event_loop()
    boot_audio = await loop.run_in_executor(None, lambda: tts.synthesize(boot_text, current_module))
    pcm_out = audio.float_to_pcm8_bytes(boot_audio)

    try:
        await websocket.send("MUTE")
    except Exception:
        pass

    pending_reply["pcm"] = pcm_out
    pending_reply["ready_at"] = time.time()
    pending_reply["delivered"] = False
    pending_reply["sent_bytes"] = 0

    await _tracked_send(websocket, pcm_out, progress=pending_reply)
    pending_reply["delivered"] = True

async def process_utterance(websocket, raw_audio, module=None, follow_up_deadline=0.0):
    if module is None:
        module = config.default_glados_module
    t0 = time.time()
    system_prompt = config.glados_modules.get(module, config.glados_modules[config.default_glados_module])

    audio_float = audio.pcm16_bytes_to_float(raw_audio)
    loop = asyncio.get_event_loop()

    tmp_wav_path = tempfile.NamedTemporaryFile(suffix=".wav", delete=False).name
    try:
        audio.save_wav_for_asr(tmp_wav_path, audio_float, config.mic_sample_rate)
        asr_result = await loop.run_in_executor(None, lambda: models.asr_model.transcribe(tmp_wav_path))
    finally:
        try:
            os.remove(tmp_wav_path)
        except OSError:
            pass

    transcript = (asr_result if isinstance(asr_result, str) else getattr(asr_result, "text", str(asr_result))).strip()
    print(f"[timing] STT: {time.time() - t0:.2f}s")

    if not transcript or stt.is_hallucination(transcript):
        print(f"[timing] TOTAL (dropped, empty/hallucination): {time.time() - t0:.2f}s")
        return follow_up_deadline

    print(f"heard: {transcript}")

    command = stt.check_wake_word(transcript)
    if command is None:
        if time.time() < follow_up_deadline:
            command = transcript
            print("no wake word, but within follow-up window - treating as command")
        else:
            print("no wake word, ignoring")
            print(f"[timing] TOTAL (dropped, no wake word): {time.time() - t0:.2f}s")
            return follow_up_deadline

    if not command:
        command = "поздоровайся"

    module_switch = stt.check_module_command(command)
    if module_switch is not None:
        try:
            await switch_module_by_voice(websocket, module_switch)
            print(f"[timing] TOTAL (module switch): {time.time() - t0:.2f}s")
            return time.time() + config.FOLLOW_UP_WINDOW_S
        except websockets.exceptions.ConnectionClosed:
            raise
        except Exception:
            print(f"[error] failed while switching module via voice to {module_switch!r}:")
            traceback.print_exc()
            try:
                await websocket.send("UNMUTE")
            except Exception:
                pass
            print(f"[timing] TOTAL (failed module switch): {time.time() - t0:.2f}s")
            return follow_up_deadline

    try:

        t_net_start = time.time()
        search_context = None
        if await loop.run_in_executor(None, llm.needs_internet_lookup, command):
            print(f"[internet] normalizer says this needs a web lookup (at {_ts()})")
            search_context = await loop.run_in_executor(None, llm.search_web, command)
            print(f"[internet] {'got results' if search_context else 'no usable results'} (at {_ts()})")
        print(f"[timing] internet check: {time.time() - t_net_start:.2f}s")

        effective_system_prompt = llm.build_effective_system_prompt(system_prompt, search_context)

        t_llm_start = time.time()
        reply_text = await loop.run_in_executor(None, llm.ask_llm, command, effective_system_prompt)
        reply_text = llm.truncate_reply(reply_text)
        print(f"[timing] LLM: {time.time() - t_llm_start:.2f}s")
        print(f"glados [{module}]: {reply_text}")

        t_tts_start = time.time()
        audio_out = await loop.run_in_executor(None, lambda: tts.synthesize(reply_text, module))
        pcm_out = audio.float_to_pcm8_bytes(audio_out)
        print(f"[timing] TTS: {time.time() - t_tts_start:.2f}s")

        try:
            await websocket.send("MUTE")
        except Exception:
            pass

        pending_reply["pcm"] = pcm_out
        pending_reply["ready_at"] = time.time()
        pending_reply["delivered"] = False
        pending_reply["sent_bytes"] = 0

        t_send_start = time.time()
        await _tracked_send(websocket, pcm_out, progress=pending_reply)
        pending_reply["delivered"] = True
        print(f"[timing] send over ws: {time.time() - t_send_start:.2f}s ({len(pcm_out)} bytes, 8-bit)")

        print(f"[timing] TOTAL round trip: {time.time() - t0:.2f}s")
        return time.time() + config.FOLLOW_UP_WINDOW_S
    except websockets.exceptions.ConnectionClosed:
        raise
    except Exception:
        print(f"[error] failed while handling utterance {command!r}:")
        traceback.print_exc()
        try:
            await websocket.send("UNMUTE")
        except Exception:
            pass
        print(f"[timing] TOTAL (failed): {time.time() - t0:.2f}s")
        return follow_up_deadline

async def _handle_client(websocket):
    global current_module
    follow_up_deadline = 0.0

    frame_bytes = int(config.mic_sample_rate * config.vad_frame_ms / 1000) * 2
    buffer = b""
    speech_frames = []
    in_speech = False
    silence_count = 0

    loop = asyncio.get_event_loop()

    async for message in websocket:
        if isinstance(message, str):
            if message.startswith("MODULE:"):
                requested = message.split(":", 1)[1].strip()
                if requested in config.glados_modules:
                    current_module = requested
                    persist_module(current_module)
                    print(f"[module] switched to: {current_module}")
                else:
                    print(f"[module] unknown module requested: {requested!r}, keeping {current_module}")

                boot_text = config.glados_boot_phrases.get(current_module, config.glados_boot_phrases[config.default_glados_module])
                print(f"[boot] glados [{current_module}]: {boot_text}")
                try:
                    boot_audio = await loop.run_in_executor(None, lambda: tts.synthesize(boot_text, current_module))
                    await _tracked_send(websocket, audio.float_to_pcm8_bytes(boot_audio))
                except Exception:
                    print(f"[boot] synth/send failed (at {_ts()}):")
                    traceback.print_exc()
            continue

        buffer += message

        while len(buffer) >= frame_bytes:
            frame = buffer[:frame_bytes]
            buffer = buffer[frame_bytes:]

            is_speech = models.vad.is_speech(frame, config.mic_sample_rate)

            if is_speech:
                speech_frames.append(frame)
                in_speech = True
                silence_count = 0
            elif in_speech:
                silence_count += 1
                speech_frames.append(frame)

                if silence_count >= config.silence_frames_to_stop:
                    if len(speech_frames) >= config.min_speech_frames:
                        follow_up_deadline = await process_utterance(
                            websocket, b"".join(speech_frames), current_module, follow_up_deadline
                        )
                    speech_frames = []
                    in_speech = False
                    silence_count = 0

async def handle_client(websocket):
    global active_websocket
    active_websocket = websocket
    print(f"esp32 connected (at {_ts()})")
    if not pending_reply["delivered"] and pending_reply["pcm"] is not None:
        age = time.time() - pending_reply["ready_at"]
        if age < config.PENDING_REPLY_RESEND_WINDOW_S:
            already_sent = pending_reply["sent_bytes"]
            remainder = pending_reply["pcm"][already_sent:]
            print(f"[resend] last reply got cut off by a disconnect ({age:.1f}s ago), "
                  f"{already_sent}/{len(pending_reply['pcm'])} bytes already delivered - "
                  f"sending the remaining {len(remainder)}")
            try:
                await _tracked_send(websocket, remainder, progress=pending_reply,
                                    base_offset=already_sent)
                pending_reply["delivered"] = True
            except Exception:
                print("[resend] failed, giving up on this one:")
                traceback.print_exc()
        else:
            print("[resend] last reply too stale to bother re-sending, dropping it")
            pending_reply["delivered"] = True

    try:
        await _handle_client(websocket)
    except websockets.exceptions.ConnectionClosed as e:
        print(f"[ws] client disconnected: {e!r} (at {_ts()})")
    except Exception:
        print("[ws] client handler crashed with an unexpected error:")
        traceback.print_exc()
    finally:
        if active_websocket is websocket:
            active_websocket = None
