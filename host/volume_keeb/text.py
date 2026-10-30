import re
import unicodedata


def normalize(name):
    """For matching an audio stream to a media player by name."""
    return re.sub(r"[^a-z0-9]", "", name.lower())


def to_ascii(text):
    """Printable ASCII only (all the knob's fonts have), accents stripped
    rather than dropped."""
    text = unicodedata.normalize("NFKD", text).encode("ascii", "ignore").decode()
    return " ".join("".join(c for c in text if c.isprintable()).split())
