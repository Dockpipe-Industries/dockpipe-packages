"""Native candidate workflow entrypoint; every location is an explicit operator input."""
import os
from pathlib import Path
import native_store


def location(name):
    value = os.environ.get(name, '')
    if not value or not Path(value).is_absolute():
        raise ValueError(name + ' must be an absolute explicit path')
    return Path(value)


if __name__ == '__main__':
    native_store.build(location('DOCKPIPE_NATIVE_LOCK'), os.environ['DOCKPIPE_NATIVE_PLATFORM'],
                       location('DOCKPIPE_NATIVE_CACHE'), location('DOCKPIPE_NATIVE_WORK'),
                       location('DOCKPIPE_NATIVE_OUTPUT'), os.environ.get('DOCKPIPE_NATIVE_FETCH') == '1')
