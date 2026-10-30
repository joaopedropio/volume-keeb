"""Windows audio: per-app volume from WASAPI's audio sessions (pycaw),
media titles and play/pause from the system media transport controls
(the media overlay's sessions, via pywinrt).

Apps are the default output's master volume plus one entry per process
with an audio session; a media session is matched to a process by name.
Windows doesn't announce volume changes the way pactl does, so this polls.
"""
import asyncio
import sys

sys.coinit_flags = 0  # COM in the multithreaded apartment, which WinRT needs too; before comtypes loads

import comtypes  # noqa: E402
from comtypes import CLSCTX_ALL  # noqa: E402
from pycaw.pycaw import AudioUtilities, IAudioEndpointVolume  # noqa: E402
from winrt.windows.media.control import (  # noqa: E402
    GlobalSystemMediaTransportControlsSessionManager as MediaManager,
    GlobalSystemMediaTransportControlsSessionPlaybackStatus as PlaybackStatus,
)

from .protocol import ACTION_PLAYPAUSE, ACTION_SET_VOLUME, ACTION_TOGGLE_MUTE, MAX_APPS  # noqa: E402
from .text import normalize, to_ascii  # noqa: E402

AUDIO_SESSION_EXPIRED = 2

# Media sessions whose app id doesn't name the process.
MEDIA_ALIASES = {
    "308046b0af4a39cb": "firefox",  # Firefox's AppUserModelID
    "microsoftzunemusic8wekyb3d8bbwemicrosoftzunemusic": "microsoftmedia",
}


def _stem(process_name):
    return process_name[:-4] if process_name.lower().endswith(".exe") else process_name


def _media_key(app_id):
    key = normalize(_stem(app_id))
    return MEDIA_ALIASES.get(key, key)


def _matches(process_key, media_key):
    if not process_key or not media_key:
        return False
    if process_key == media_key:
        return True
    shorter = min(process_key, media_key, key=len)
    return len(shorter) >= 4 and (process_key in media_key or media_key in process_key)


class Audio:
    poll_s = 0.5

    def __init__(self):
        self.loop = None
        self.media_manager = None
        self.endpoint = None

    def start(self, wake):
        pass  # nothing to subscribe to; the engine polls every poll_s

    def thread_init(self):
        """Called once on the thread that then calls collect()/apply()."""
        comtypes.CoInitializeEx(comtypes.COINIT_MULTITHREADED)
        self.loop = asyncio.new_event_loop()

    def _endpoint(self):
        if self.endpoint is None:
            speakers = AudioUtilities.GetSpeakers()
            endpoint = getattr(speakers, "EndpointVolume", None)  # pycaw >= 20240316
            if endpoint is None:
                iface = speakers.Activate(IAudioEndpointVolume._iid_, CLSCTX_ALL, None)
                endpoint = iface.QueryInterface(IAudioEndpointVolume)
            self.endpoint = endpoint
        return self.endpoint

    def _read_master(self):
        try:
            ep = self._endpoint()
            volume = round(ep.GetMasterVolumeLevelScalar() * 100)
            muted = bool(ep.GetMute())
        except comtypes.COMError:
            self.endpoint = None  # default device changed; look it up again next time
            volume, muted = 0, False
        return {"master": True, "name": "Master", "volume": volume, "muted": muted, "title": ""}

    def _read_sessions(self):
        """process name -> [session, ...], in the order Windows lists them."""
        groups = {}
        for s in AudioUtilities.GetAllSessions():
            if s.Process is None or s.State == AUDIO_SESSION_EXPIRED:
                continue  # system sounds, or a stream that's gone
            try:
                name = s.Process.name()
            except Exception:  # psutil: the process just exited
                continue
            groups.setdefault(name, []).append(s)
        return groups

    async def _read_media(self):
        if self.media_manager is None:
            self.media_manager = await MediaManager.request_async()
        media = []
        for session in self.media_manager.get_sessions():
            try:
                props = await session.try_get_media_properties_async()
                status = session.get_playback_info().playback_status
            except OSError:
                continue
            media.append({
                "key": _media_key(session.source_app_user_model_id),
                "session": session,
                "title": to_ascii(props.title or ""),
                "playing": status == PlaybackStatus.PLAYING,
            })
        return media

    def collect(self):
        apps = [self._read_master()]
        try:
            media = self.loop.run_until_complete(self._read_media())
        except OSError:
            self.media_manager = None
            media = []
        for process_name, sessions in self._read_sessions().items():
            first = sessions[0]
            display = first.DisplayName
            name = display if display and not display.startswith("@") else _stem(process_name).capitalize()
            vol = first.SimpleAudioVolume
            app = {
                "master": False,
                "name": to_ascii(name),
                "volume": round(vol.GetMasterVolume() * 100),
                "muted": bool(vol.GetMute()),
                "title": "",
                "sessions": sessions,
            }
            key = normalize(_stem(process_name))
            for m in media:
                if _matches(key, m["key"]):
                    app.update(title=m["title"], playing=m["playing"], media_session=m["session"])
                    break
            apps.append(app)
        return apps[:MAX_APPS]

    def apply(self, app, action, value):
        if action == ACTION_PLAYPAUSE:
            if app.get("media_session") is not None:
                self.loop.run_until_complete(app["media_session"].try_toggle_play_pause_async())
            return
        if app["master"]:
            ep = self._endpoint()
            if action == ACTION_SET_VOLUME:
                ep.SetMasterVolumeLevelScalar(value / 100, None)
            elif action == ACTION_TOGGLE_MUTE:
                ep.SetMute(not ep.GetMute(), None)
            return
        muted = not app["muted"]
        for s in app["sessions"]:
            vol = s.SimpleAudioVolume
            if action == ACTION_SET_VOLUME:
                vol.SetMasterVolume(value / 100, None)
            elif action == ACTION_TOGGLE_MUTE:
                vol.SetMute(muted, None)
