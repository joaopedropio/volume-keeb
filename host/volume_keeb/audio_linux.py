"""Linux audio: per-app streams from `pactl` (PulseAudio or PipeWire's
pulse server), media titles and play/pause from MPRIS players via `busctl`.

Apps are `pactl list sink-inputs` (one entry per playing stream) plus a
synthetic "Master" entry for the default sink; an MPRIS player is matched
to a sink-input by application name.
"""
import json
import re
import subprocess
import threading
import time

from .protocol import ACTION_PLAYPAUSE, ACTION_SET_VOLUME, ACTION_TOGGLE_MUTE, MAX_APPS, HEARTBEAT_S
from .text import normalize, to_ascii


def _run(*cmd):
    return subprocess.run(cmd, capture_output=True, text=True).stdout


def busctl(*args):
    return _run("busctl", "--user", *args)


def read_master():
    vol = _run("pactl", "get-sink-volume", "@DEFAULT_SINK@")
    mute = _run("pactl", "get-sink-mute", "@DEFAULT_SINK@")
    match = re.search(r"(\d+)%", vol)
    volume = min(int(match.group(1)), 100) if match else 0
    return {"master": True, "name": "Master", "volume": volume, "muted": "yes" in mute, "title": ""}


def read_sink_inputs():
    """One dict per playing sink-input, parsed from `pactl list sink-inputs`."""
    apps = []
    cur = None
    for line in _run("pactl", "list", "sink-inputs").splitlines():
        m = re.match(r"Sink Input #(\d+)", line)
        if m:
            cur = {"master": False, "index": int(m.group(1)), "name": None, "volume": 0, "muted": False, "title": ""}
            apps.append(cur)
            continue
        if cur is None:
            continue
        stripped = line.strip()
        m = re.match(r"Volume:.*?(\d+)%", stripped)
        if m:
            cur["volume"] = min(int(m.group(1)), 100)
        m = re.match(r"Mute:\s*(yes|no)", stripped)
        if m:
            cur["muted"] = m.group(1) == "yes"
        m = re.match(r'application\.name\s*=\s*"(.*)"', stripped)
        if m:
            cur["name"] = to_ascii(m.group(1))
    return [a for a in apps if a["name"]]


def read_players():
    """normalized identity -> {bus, playing, title}, one per MPRIS player."""
    players = {}
    for line in busctl("list", "--acquired").splitlines():
        name = line.split(" ", 1)[0]
        if not name.startswith("org.mpris.MediaPlayer2."):
            continue
        prop_root = ["get-property", name, "/org/mpris/MediaPlayer2"]
        try:
            identity = json.loads(busctl(*prop_root, "org.mpris.MediaPlayer2", "Identity"))["data"]
        except (ValueError, KeyError):
            identity = name.rsplit(".", 1)[-1]
        prop = [*prop_root, "org.mpris.MediaPlayer2.Player"]
        playing = '"Playing"' in busctl(*prop, "PlaybackStatus")
        title = ""
        try:
            metadata = json.loads(busctl("--json=short", *prop, "Metadata"))["data"]
            title = metadata.get("xesam:title", {}).get("data", "")
        except (ValueError, KeyError):
            pass
        players[normalize(identity)] = {"bus": name, "playing": playing, "title": to_ascii(title)}
    return players


class Audio:
    # pactl announces volume/stream changes itself; this is just how often
    # media titles (which it doesn't announce) get picked up.
    poll_s = HEARTBEAT_S

    def start(self, wake):
        """Calls wake() on every sink/server event, from a thread of its own."""
        threading.Thread(target=self._subscribe, args=(wake,), daemon=True).start()

    def _subscribe(self, wake):
        while True:
            sub = subprocess.Popen(["pactl", "subscribe"], stdout=subprocess.PIPE, text=True)
            try:
                for line in sub.stdout:
                    if "sink" in line or "server" in line:
                        wake()
            finally:
                sub.kill()
            time.sleep(1)  # audio server restarted; resubscribe

    def collect(self):
        """Master plus up to MAX_APPS-1 sink-inputs, each matched to a player."""
        apps = [read_master()]
        players = read_players()
        for app in read_sink_inputs():
            player = players.get(normalize(app["name"]))
            if player:
                app["title"] = player["title"]
                app["playing"] = player["playing"]
                app["player_bus"] = player["bus"]
            apps.append(app)
        return apps[:MAX_APPS]

    def apply(self, app, action, value):
        target = "@DEFAULT_SINK@" if app["master"] else str(app["index"])
        if action == ACTION_SET_VOLUME:
            cmd = "set-sink-volume" if app["master"] else "set-sink-input-volume"
            _run("pactl", cmd, target, f"{value}%")
        elif action == ACTION_TOGGLE_MUTE:
            cmd = "set-sink-mute" if app["master"] else "set-sink-input-mute"
            _run("pactl", cmd, target, "toggle")
        elif action == ACTION_PLAYPAUSE and app.get("player_bus"):
            busctl("call", app["player_bus"], "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player", "PlayPause")
