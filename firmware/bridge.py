import sys

sys.coinit_flags = 0

import asyncio
import contextlib
import json

from pycaw.pycaw import AudioUtilities

from winrt.windows.media.control import(GlobalSystemMediaTransportControlsSessionManager, GlobalSystemMediaTransportControlsSessionPlaybackStatus,)

from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed

HOST = "0.0.0.0"
PORT = 8765
STATUS_INTERVAL = 1.0

def spotify_audio_sessions():
    """Find Spotify audio sessions on the default ouput device."""
    matches = []

    for session in AudioUtilities.GetAllSessions():
        try:
            process = session.Process

            if process and process.name().lower() == "spotify.exe":
                matches.append(session)
        except Exception:
            continue
    return matches

def get_spotify_volume():
    sessions = spotify_audio_sessions()

    if not sessions:
        return None

    level = sessions[0].SimpleAudioVolume.GetMasterVolume()
    return max(0, min(100, round(level * 100)))

def set_spotify_volume(value):
    value = max(0, min(100, value))
    sessions = spotify_audio_sessions()

    if not sessions:
        print("Volume ignored: no local Spotify audio session found.")
        return False

    for session in sessions:
        session.SimpleAudioVolume.SetMasterVolume(value / 100.0, None)

    return True

def find_spotify_media_session(manager):
    for session in manager.get_sessions():
        app_id = session.source_app_user_model_id or ""

        if "spotify" in app_id.lower():
            return session
        
    return None

async def get_status(manager):
    session = find_spotify_media_session(manager)

    status = {
        "connected": True,
        "title": "Spotify not active",
        "artist": "Open Spotify and play a song",
        "playing": False,
    }

    if session is not None:
        properties = await session.try_get_media_properties_asynch()
        playback = session.get_playback_info()

        status["title"] = properties.title or "No track title"
        status["artist"] = properties.artist or ""

        status["playing"] = (playback.playback_status == GlobalSystemMediaTransportControlsSessionPlaybackStatus.PLAYING)

    volume = get_spotify_volume()

    if volume is not None:
        status["volume"] = volume

    return status

async def execute_command(manager, message):
    command = message.get("command")

    if command == "volume":
        value = message.get("value")

        if type(value) is not int:
            print("Volume ignored: value must be an integer.")
            return

        set_spotify_volume(value)
        return

    if command not in ("previous", "toggle", "next"):
        print(f"Unknown command ignored: {command!r}")
        return

    session = find_spotify_media_session(manager)

    if session is None:
        print("Command ignored: no Spotify media session found.")
        return

    if command == "previous":
        accepted = await session.try_skip_previous_async()
    elif command == "toggle":
        accepted = await session.try_toggle_play_pause_async()
    else:
        accepted = await session.try_skip_next_async()

    print(f"{command}: {'accepted' if accepted else 'not accepted'}")

async def send_status_forever(websocket, manager):
    while True:
        try:
            status = await get_status(manager)
        except Exception as error:
            print(f"Status read failed: {error}")

            status = {
                "connected": True,
                "title": "Spotify status unavailable",
                "artist": "Check the bridge console",
                "playing": False,
            }

        message = json.dumps(status, ensure_ascii=True)

        if len(message.encode("utf-8")) >= 2048:
            status["title"] = status["title"][:80]
            status["artist"] = status["artist"][:80]
            message = json.dumps(status, ensure_ascii=True)

        await websocket.send(message)
        await asyncio.sleep(STATUS_INTERVAL)

async def handle_client(websocket, manager):
    if websocket.request.path != "/":
        await websocket.close(code=1008, reason="Unsupported path")
        return

    print(f"Controller connected: {websocket.remote_address}")

    sender = asyncio.create.tast(send_status_forever(websocket, manager))

    def sender_finished(task):
        if not task.cancelled():
            asyncio.create.task(websocket.close())

    sender.add_done_callback(sender_finished)

    try:
        async for raw_message in websocket:
            try:
                message = json.load(raw_message)

                if not isinstance(message, dict):
                    continue

                await execute_command(manager, message)

            except (json.JSONDecodeError, TypeError, ValueError):
                print("Invalid command ignored.")

            except Exception as error:
                print(f"Command failed: {error}")

    except ConnectionClosed:
        pass

    finally:
        sender.cancel()

        with contextlib.suppress(asyncio.CancelledError, ConnectionClosed):
            await sender

        print("Controller disconnected.")

async def main():
    manager = (await GlobalSystemMediaTransportControlsSessionManager.request_async())

    print(f"Bridge listened on port {PORT}")
    print("Open the Spotify desktop app and play a song.")
    print("Leave this window running. Press Ctrl+C to stop.")

    async def handler(websocket):
        await handle_client(websocket, manager)

    async with serve(handler, HOST, PORT, max_size=2048, ping_interval=20,ping_timeout=20,):
        await asyncio.Future()

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nBridge stopped.")
