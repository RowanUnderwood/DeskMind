"""Render every MindServer tab off-screen to out\\gui_<tab>.png (layout check).

    set QT_QPA_PLATFORM=offscreen & .venv\\Scripts\\python -m tests.gui_shots
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from PySide6.QtCore import QTimer  # noqa: E402
from PySide6.QtWidgets import QApplication  # noqa: E402

from mindserver.config import HELPER_DIR, Config  # noqa: E402
from mindserver.gui import MainWindow  # noqa: E402
from mindserver.server import MindServer  # noqa: E402


def main():
    app = QApplication([])
    cfg = Config()
    cfg.path = os.path.join(HELPER_DIR, "out", "settings_test.json")   # never touch the real settings
    cfg.data["server"]["port"] = 8289                 # don't clash with a running MindServer
    server = MindServer(cfg)
    server.start()
    win = MainWindow(cfg, server)
    win.resize(1320, 820)
    win.show()
    out = os.path.join(HELPER_DIR, "out")
    names = [win.tabs.tabText(i) for i in range(win.tabs.count())]

    def shoot():
        for i, name in enumerate(names):
            win.tabs.setCurrentIndex(i)
            app.processEvents()
            win.grab().save(os.path.join(out, f"gui_{name.replace(' ', '_').lower()}.png"))
        print("saved", names)
        server.stop()
        app.quit()

    start = time.time()

    def wait_lab():
        if (win.lab.busy or win.lab.tandy.pixmap() is None or win.lab.tandy.pixmap().isNull()) and time.time() - start < 30:
            QTimer.singleShot(300, wait_lab)
        else:
            shoot()

    QTimer.singleShot(1500, wait_lab)
    app.exec()


if __name__ == "__main__":
    main()
