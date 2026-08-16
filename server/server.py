import os
import asyncio

from dotenv import load_dotenv

load_dotenv()
os.environ.setdefault("HF_HUB_DISABLE_SYMLINKS_WARNING", "1")

from glados import platform_fixes
platform_fixes.add_nvidia_dlls_to_path()
platform_fixes.fix_windows_console_encoding()

import websockets

from glados import config
from glados import console
from glados import models
from glados.session import handle_client

async def main():
    async with websockets.serve(
        handle_client, config.ws_host, config.ws_port, max_size=None, ping_interval=20, ping_timeout=60
    ):
        print(f"listening on ws://{config.ws_host}:{config.ws_port}")
        asyncio.create_task(console.console_command_loop())
        await asyncio.Future()

if __name__ == "__main__":
    asyncio.run(main())
