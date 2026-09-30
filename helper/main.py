"""MindServer entry point.

    python main.py            GUI + server
    python main.py --nogui    server only (console)
"""

import argparse
import logging
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
