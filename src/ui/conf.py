"""
conf.py — read enkakumon.conf (KEY = value lines, # comments).

Same rules as src/conf.c: blank lines and comments are skipped; a line
without '=', or with an empty key or value, is an error. Values that parse
as int or float become numbers; everything else stays a string.

Acceptance tests:
  python3 -c "import conf; print(conf.load())" (from src/ui) prints every key
  in the shipped enkakumon.conf; a line without '=' raises ConfError naming
  its line number; a missing file raises ConfError.
"""
import os

DEFAULT_PATH = os.path.normpath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "enkakumon.conf"))


class ConfError(Exception):
    pass


def _convert(value):
    for cast in (int, float):
        try:
            return cast(value)
        except ValueError:
            pass
    return value


def load(path=DEFAULT_PATH):
    """Return the config as a dict. Raises ConfError on any problem."""
    conf = {}
    try:
        f = open(path, encoding="utf-8")
    except OSError as e:
        raise ConfError(f"cannot open {path}: {e.strerror}") from e
    with f:
        for lineno, line in enumerate(f, 1):
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            key, sep, value = line.partition("=")
            key, value = key.strip(), value.strip()
            if not sep or not key or not value:
                raise ConfError(f"{path}:{lineno}: expected KEY = value")
            conf[key] = _convert(value)
    return conf
