"""Send one message through the AI tab's test chat (off-screen), save out\\gui_chat.png.
Needs NInfer running.   set QT_QPA_PLATFORM=offscreen & .venv\\Scripts\\python -m tests.gui_chat
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
    ai = win.ai
    win.tabs.setCurrentWidget(ai)
    ai.msg.setText("Give me three fun facts about the Tandy 1000, as a short list.")
    start = time.time()

    def poll():
        if ai.chat_id or time.time() - start > 120:
            app.processEvents()
            win.grab().save(os.path.join(HELPER_DIR, "out", "gui_chat.png"))
            print("chat", ai.chat_id, f"{time.time() - start:.1f}s")
            server.chat.chats.delete(ai.chat_id or "")
            server.stop()
            app.quit()
        else:
            QTimer.singleShot(250, poll)

    QTimer.singleShot(500, lambda: (ai.send(), poll()))
    app.exec()


if __name__ == "__main__":
    main()
