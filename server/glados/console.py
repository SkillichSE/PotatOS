import asyncio
import os
import sys
import time
import traceback

from . import audio
from . import config
from . import session

async def play_mp3_command():
    ws = session.active_websocket
    if ws is None:
        print("[mp3] no ESP32 connected right now")
        return
    if not os.path.isfile(config.MP3_PLAY_PATH):
        print(f"[mp3] file not found: {config.MP3_PLAY_PATH}")
        return

    loop = asyncio.get_event_loop()
    try:
        print(f"[mp3] decoding {config.MP3_PLAY_PATH}")
        pcm = await loop.run_in_executor(None, audio.load_mp3_as_pcm8, config.MP3_PLAY_PATH)
    except Exception:
        print(f"[mp3] failed to decode {config.MP3_PLAY_PATH}:")
        traceback.print_exc()
        return

    session.pending_reply["pcm"] = pcm
    session.pending_reply["ready_at"] = time.time()
    session.pending_reply["delivered"] = False
    session.pending_reply["sent_bytes"] = 0

    try:
        await ws.send("MUTE")
    except Exception:
        print("[mp3] couldn't reach the ESP32 (send failed) - not connected anymore?")
        return

    try:
        print(f"[mp3] sending {len(pcm)} bytes")
        await session._tracked_send(ws, pcm, progress=session.pending_reply)
        session.pending_reply["delivered"] = True
        print("[mp3] done")
    except asyncio.CancelledError:
        print("[mp3] stopped")
    except Exception:
        print("[mp3] interrupted, probably a disconnect - remainder will resend on reconnect:")
        traceback.print_exc()

async def console_command_loop():
    loop = asyncio.get_event_loop()
    print(f"[console] type 'play' + Enter to send {config.MP3_PLAY_PATH} to the ESP32")
    print("[console] type 'stop' + Enter to stop playback")
    while True:
        line = await loop.run_in_executor(None, sys.stdin.readline)
        if not line:
            await asyncio.sleep(3600)
            continue
        cmd = line.strip().lower()
        if cmd == "play":
            await play_mp3_command()
        elif cmd == "stop":
            await session.stop_playback()
            print("[console] stopped")
        elif cmd:
            print(f"[console] unknown command: {cmd!r} (try 'play' or 'stop')")
