"""Click Generate in the GUI (off-screen), wait for the result, save out\\gui_generated.png.
Needs ComfyUI running.   set QT_QPA_PLATFORM=offscreen & .venv\\Scripts\\python -m tests.gui_gen
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
    cfg.data["server"]["port"] = 8289
    server = MindServer(cfg)
    server.start()
    win = MainWindow(cfg, server)
    win.resize(1320, 820)
    win.show()
    g = win.generation
    win.tabs.setCurrentWidget(g)
    g.prompt.setPlainText("A brave knight facing a small friendly dragon on a hill at sunset, "
                          "bold shapes, strong contrast, simple sky, 1980s adventure game box art")
    start = time.time()
    seen = []

    def poll():
        seen.append(g.state.text())
        if g.go.isEnabled() and g.state.text().startswith(("I ", "E ")) or time.time() - start > 180:
            win.grab().save(os.path.join(HELPER_DIR, "out", "gui_generated.png"))
            print("final:", g.state.text(), f"({time.time() - start:.1f}s, {len(set(seen))} distinct states)")
            print("gallery items:", win.gallery.list.count())
            server.stop()
            app.quit()
        else:
            QTimer.singleShot(250, poll)

    QTimer.singleShot(500, lambda: (g.go.click(), poll()))
    app.exec()


if __name__ == "__main__":
    main()
