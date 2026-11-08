#!/usr/bin/env python3
"""Send one small nonblocking command to an existing Linux audio session."""
import argparse
import os
from pathlib import Path
import sys

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-dir", default="/tmp/dm8168-spatial-audio")
    parser.add_argument("command", choices=["start", "stop", "direction", "bypass"])
    parser.add_argument("value", nargs="?", choices=["0", "1"])
    args = parser.parse_args()
    if (args.command in ("direction", "bypass")) != (args.value is not None):
        parser.error("direction/bypass require 0 or 1; start/stop take no value")
    message = args.command.upper() + (" " + args.value if args.value is not None else "") + "\n"
    try:
        fd = os.open(str(Path(args.runtime_dir) / "control.fifo"), os.O_WRONLY | os.O_NONBLOCK)
        try:
            if os.write(fd, message.encode("ascii")) != len(message):
                raise OSError("short command write")
        finally:
            os.close(fd)
    except OSError as error:
        parser.exit(1, "No connected backend: {}\n".format(error))
    return 0

if __name__ == "__main__":
    sys.exit(main())
