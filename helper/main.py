"""MindServer entry point.

    python main.py            GUI + server
    python main.py --nogui    server only (console)
"""

import argparse
import logging
import logging.handlers
import os
import sys
import time

from mindserver.config import Config
from mindserver.server import MindServer


def main() -> int:
    ap = argparse.ArgumentParser(description="MindServer for DeskMind")
    ap.add_argument("--nogui", action="store_true", help="run without the Qt window")
    ap.add_argument("--port", type=int, help="override the listening port")
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    config = Config()
    if args.port:
        config.data["server"]["port"] = args.port

    # The same log in out\mindserver.log, so an intermittent failure on the Tandy can be matched to what
    # MindServer saw (each request line has the Tandy's port; DeskMind's NET.LOG has the time).
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
    os.makedirs(out, exist_ok=True)
    name = "mindserver.log" if not args.port else f"mindserver{args.port}.log"
    fh = logging.handlers.RotatingFileHandler(os.path.join(out, name), maxBytes=2_000_000, backupCount=3,
                                              encoding="utf-8")
    fh.setFormatter(logging.Formatter("%(asctime)s.%(msecs)03d %(levelname)s %(name)s %(message)s",
                                      "%Y-%m-%d %H:%M:%S"))
    logging.getLogger().addHandler(fh)

    server = MindServer(config)
    server.start()

    gui = None
    if not args.nogui:
        try:
            from mindserver.gui import run_gui as gui
        except ImportError:
            logging.info("GUI not available yet - running in console mode (Ctrl+C stops)")

    if gui is None:
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            pass
        server.stop()
        return 0

    return gui(config, server)


if __name__ == "__main__":
    sys.exit(main())
