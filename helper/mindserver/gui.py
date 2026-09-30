"""MindServer window (PySide6).  A native desktop window on purpose: a browser
tab would take VRAM on GPU 0, which NInfer needs.

Tabs: Services, Generation, Dither Lab, Gallery, AI, Tandy.
"""

from __future__ import annotations

import dataclasses
import logging
import os
import time

from PIL import Image
from PySide6.QtCore import QObject, QRunnable, Qt, QThreadPool, QTimer, Signal
from PySide6.QtGui import QColor, QIcon, QImage, QPixmap
from PySide6.QtWidgets import (QApplication, QCheckBox, QColorDialog, QComboBox, QDoubleSpinBox,
                               QFileDialog, QFormLayout, QGridLayout, QGroupBox, QHBoxLayout,
                               QInputDialog, QLabel, QLineEdit, QListWidget, QListWidgetItem,
                               QMainWindow, QMessageBox, QPlainTextEdit, QProgressBar, QPushButton,
                               QScrollArea, QSlider, QSpinBox, QSplitter, QTabWidget, QVBoxLayout,
                               QWidget)

from . import dither as D
from . import tpi
from .config import HELPER_DIR, PROJECT_DIR, Config
from .server import VERSION, MindServer

log = logging.getLogger("mindserver.gui")

DOT = {"up": "#2ecc40", "down": "#ff4136", "unknown": "#aaaaaa"}


def to_pixmap(img: Image.Image) -> QPixmap:
    img = img.convert("RGB")
    data = img.tobytes()
    q = QImage(data, img.width, img.height, img.width * 3, QImage.Format.Format_RGB888)
    return QPixmap.fromImage(q.copy())


def fit_43(img: Image.Image, width: int = 640) -> Image.Image:
    return D.fit_4x3(img, "crop").resize((width, width * 3 // 4), Image.Resampling.LANCZOS)


class Bridge(QObject):
    """Carries events from worker threads into the GUI thread."""
    status = Signal(dict)
    job = Signal(object)
    log_line = Signal(str)


class QtLogHandler(logging.Handler):
    def __init__(self, bridge: Bridge):
        super().__init__()
        self.bridge = bridge
        self.setFormatter(logging.Formatter("%(asctime)s %(message)s", "%H:%M:%S"))

    def emit(self, record):
        try:
            self.bridge.log_line.emit(self.format(record))
        except RuntimeError:
            pass


class Task(QRunnable):
    """Run fn in the thread pool and deliver the result through a signal."""

    class Signals(QObject):
        done = Signal(object, object)          # result, error

    def __init__(self, fn):
        super().__init__()
        self.fn = fn
        self.signals = Task.Signals()

    def run(self):
        try:
            self.signals.done.emit(self.fn(), None)
        except Exception as e:                  # shown in the GUI
            self.signals.done.emit(None, e)


def slider(lo: int, hi: int, value: int) -> QSlider:
    s = QSlider(Qt.Orientation.Horizontal)
    s.setRange(lo, hi)
    s.setValue(value)
    return s


# ====================================================================== Services

class ServicesTab(QWidget):
    def __init__(self, win: "MainWindow"):
        super().__init__()
        self.win = win
        lay = QVBoxLayout(self)
        box = QGroupBox("Services")
        g = QGridLayout(box)
        self.dots = {}
        rows = [("qwen", "NInfer / Qwen (RTX 5090)", win.config["qwen"]["url"]),
                ("comfy", "ComfyUI / Krea2 (RTX 4090)", win.config["comfy"]["url"])]
        for r, (key, name, url) in enumerate(rows):
            dot = QLabel("●")
            self.dots[key] = dot
            g.addWidget(dot, r, 0)
            g.addWidget(QLabel(f"<b>{name}</b>"), r, 1)
            g.addWidget(QLabel(url), r, 2)
            b1, b2 = QPushButton("Start"), QPushButton("Stop")
            b1.clicked.connect(lambda _=False, k=key: self.start(k))
            b2.clicked.connect(lambda _=False, k=key: self.stop(k))
            g.addWidget(b1, r, 3)
            g.addWidget(b2, r, 4)
        s = win.config["server"]
        g.addWidget(QLabel("●"), 2, 0)
        self.dot_server = g.itemAtPosition(2, 0).widget()
        self.dot_server.setStyleSheet(f"color: {DOT['up']}; font-size: 16px")
        g.addWidget(QLabel(f"<b>MindServer {VERSION}</b>"), 2, 1)
        g.addWidget(QLabel(f"port {s['port']}  (DeskMind connects here)"), 2, 2)
        note = QLabel("NInfer needs about 22 GB free on GPU 0; start it first, from a clean boot if it refuses.")
        note.setStyleSheet("color: #888")
        g.addWidget(note, 3, 1, 1, 4)
        g.setColumnMinimumWidth(1, 220)
        g.setColumnMinimumWidth(2, 260)
        for col in (3, 4):
            for row in (0, 1):
                g.itemAtPosition(row, col).widget().setFixedWidth(90)
        g.setColumnStretch(5, 1)
        lay.addWidget(box)

        split = QSplitter()
        self.logview = QPlainTextEdit()
        self.logview.setReadOnly(True)
        self.logview.setMaximumBlockCount(3000)
        lg = QGroupBox("Log")
        QVBoxLayout(lg).addWidget(self.logview)
        self.clients = QListWidget()
        cg = QGroupBox("Tandy clients")
        QVBoxLayout(cg).addWidget(self.clients)
        split.addWidget(lg)
        split.addWidget(cg)
        split.setSizes([700, 200])
        lay.addWidget(split, 1)

        self.set_status(win.server.status())
        t = QTimer(self)
        t.timeout.connect(self.refresh_clients)
        t.start(2000)

    def set_status(self, st: dict):
        for k, dot in self.dots.items():
            dot.setStyleSheet(f"color: {DOT.get(st.get(k), '#e0a000')}; font-size: 16px")
            dot.setToolTip(st.get(k, "?"))

    def refresh_clients(self):
        now = time.time()
        self.clients.clear()
        for ip, seen in sorted(self.win.server.clients.items(), key=lambda kv: -kv[1]):
            self.clients.addItem(f"{ip}   {int(now - seen)} s ago")

    def start(self, key):
        name = "NInfer" if key == "qwen" else "ComfyUI"
        if self.win.server.services.status.get(key) == "up":
            log.info("%s is already running - not starting a second copy", name)
            return
        log.info("starting %s", name)
        self.win.server.services.start(key)

    def stop(self, key):
        if QMessageBox.question(self, "Stop", f"Stop {'NInfer' if key == 'qwen' else 'ComfyUI'}?") \
                == QMessageBox.StandardButton.Yes:
            log.info("stopping %s", key)
            self.win.server.services.stop(key)


# ====================================================================== Generation

class GenerationTab(QWidget):
    def __init__(self, win: "MainWindow"):
        super().__init__()
        self.win = win
        c = win.config["comfy"]
        lay = QHBoxLayout(self)
        left = QVBoxLayout()
        box = QGroupBox("ComfyUI settings")
        f = QFormLayout(box)
        self.url = QLineEdit(c["url"])
        self.workflow = QLineEdit(c["workflow"])
        self.steps = QSpinBox(); self.steps.setRange(1, 60); self.steps.setValue(c["steps"])
        self.shortside = QSpinBox(); self.shortside.setRange(384, 2048); self.shortside.setSingleStep(64)
        self.shortside.setValue(c["shortside"])
        self.lora_on = QCheckBox("KNPV3_1 LoRA on"); self.lora_on.setChecked(c["lora_on"])
        self.lora_strength = QDoubleSpinBox(); self.lora_strength.setRange(0, 2); self.lora_strength.setSingleStep(0.1)
        self.lora_strength.setValue(c["lora_strength"])
        f.addRow("URL", self.url)
        f.addRow("Workflow", self.workflow)
        f.addRow("Steps", self.steps)
        f.addRow("Short side (px)", self.shortside)
        f.addRow(self.lora_on)
        f.addRow("LoRA strength", self.lora_strength)
        save = QPushButton("Save settings")
        save.clicked.connect(self.save)
        f.addRow(save)
        left.addWidget(box)

        tb = QGroupBox("Test generation (goes into the Gallery, and the Tandy can fetch it)")
        tl = QVBoxLayout(tb)
        self.prompt = QPlainTextEdit("A friendly retro computer logo for '286 AI', bold shapes, strong contrast")
        self.prompt.setMaximumHeight(90)
        tl.addWidget(self.prompt)
        row = QHBoxLayout()
        self.mode = QComboBox(); self.mode.addItems(["640", "320"])
        self.mode.setCurrentText(win.config["dither"]["mode"])
        self.go = QPushButton("Generate")
        self.go.clicked.connect(self.generate)
        row.addWidget(QLabel("Mode")); row.addWidget(self.mode); row.addStretch(); row.addWidget(self.go)
        tl.addLayout(row)
        self.bar = QProgressBar()
        self.state = QLabel("")
        tl.addWidget(self.bar)
        tl.addWidget(self.state)
        left.addWidget(tb)
        left.addStretch()
        lay.addLayout(left, 1)

        self.preview = QLabel("The result appears here, as it will look on the Tandy.")
        self.preview.setMinimumSize(640, 480)
        self.preview.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.preview.setStyleSheet("background: #111; color: #888")
        lay.addWidget(self.preview, 2)
        self.job_id = None

    def save(self):
        c = self.win.config["comfy"]
        c.update(url=self.url.text().strip(), workflow=self.workflow.text().strip(), steps=self.steps.value(),
                 shortside=self.shortside.value(), lora_on=self.lora_on.isChecked(),
                 lora_strength=round(self.lora_strength.value(), 2))
        self.win.config.save()
        log.info("generation settings saved")

    def generate(self):
        self.save()
        text = self.prompt.toPlainText().strip()
        if not text:
            return
        job = self.win.server.jobs.submit(text, self.mode.currentText())
        self.job_id = job.id
        self.go.setEnabled(False)
        self.bar.setValue(0)

    def on_job(self, job):
        if job.id != self.job_id:
            return
        self.bar.setValue(int(job.progress * 100))
        self.state.setText(job.line())
        if job.status in ("done", "error"):
            self.go.setEnabled(True)
        if job.status == "done":
            data = self.win.server.store.tpi_bytes(job.image_id)
            info = tpi.parse(data)
            idx = D.unpack(info.image, info.width, info.height)
            self.preview.setPixmap(to_pixmap(D.preview_4x3(idx, self.win.config.dither_settings(), 640)))
            self.win.gallery.refresh()


# ====================================================================== Dither Lab

class DitherLabTab(QWidget):
    def __init__(self, win: "MainWindow"):
        super().__init__()
        self.win = win
        self.source: Image.Image | None = None
        self.source_name = ""
        self.presets: dict[str, dict] = {}
        self.pool = QThreadPool.globalInstance()
        self.busy = False
        self.dirty = False
        self._loading = False

        lay = QHBoxLayout(self)
        ctl = QVBoxLayout()

        src = QGroupBox("Source image")
        sl = QVBoxLayout(src)
        row = QHBoxLayout()
        b_open = QPushButton("Open file…"); b_open.clicked.connect(self.open_file)
        b_last = QPushButton("Latest generated"); b_last.clicked.connect(self.load_latest)
        row.addWidget(b_open); row.addWidget(b_last)
        sl.addLayout(row)
        self.src_label = QLabel("(none)")
        sl.addWidget(self.src_label)
        ctl.addWidget(src)

        box = QGroupBox("Settings")
        f = QFormLayout(box)
        self.mode = QComboBox(); self.mode.addItems(["640", "320"])
        self.fit = QComboBox(); self.fit.addItems(["crop", "letterbox", "stretch"])
        self.engine = QComboBox(); self.engine.addItems(D.ENGINES)
        self.method = QComboBox()
        self.order = QSpinBox(); self.order.setRange(2, 16)
        self.threshold = slider(0, 255, 64)
        self.strength = slider(0, 100, 80)
        self.serpentine = QCheckBox("serpentine")
        self.brightness = slider(0, 200, 100)
        self.contrast = slider(0, 200, 115)
        self.saturation = slider(0, 250, 130)
        self.gamma = slider(40, 250, 100)
        self.sharpen = slider(0, 200, 60)
        self.brown = QCheckBox("colour 6 = brown (off: dark yellow)")
        for label, w in [("Mode", self.mode), ("Fit to 4:3", self.fit), ("Engine", self.engine),
                         ("Method", self.method), ("Matrix order", self.order), ("Threshold", self.threshold),
                         ("Strength", self.strength), ("", self.serpentine), ("Brightness", self.brightness),
                         ("Contrast", self.contrast), ("Saturation", self.saturation), ("Gamma", self.gamma),
                         ("Sharpen", self.sharpen), ("", self.brown)]:
            f.addRow(label, w)
        ctl.addWidget(box)

        pb = QGroupBox("Presets")
        pl = QGridLayout(pb)
        for i, name in enumerate(("A", "B")):
            s = QPushButton(f"Store {name}"); s.clicked.connect(lambda _=False, n=name: self.store_preset(n))
            l = QPushButton(f"Show {name}"); l.clicked.connect(lambda _=False, n=name: self.load_preset(n))
            pl.addWidget(s, 0, i); pl.addWidget(l, 1, i)
        ctl.addWidget(pb)

        act = QGroupBox("Use")
        al = QVBoxLayout(act)
        b_def = QPushButton("Save as default for new images")
        b_def.clicked.connect(self.save_default)
        b_send = QPushButton("Send this image to the Tandy (Gallery)")
        b_send.clicked.connect(self.send_to_tandy)
        b_reset = QPushButton("Reset to saved default")
        b_reset.clicked.connect(lambda: self.load_settings(self.win.config.dither_settings()))
        al.addWidget(b_def); al.addWidget(b_send); al.addWidget(b_reset)
        ctl.addWidget(act)
        self.timing = QLabel("")
        ctl.addWidget(self.timing)
        ctl.addStretch()
        wrap = QWidget(); wrap.setLayout(ctl); wrap.setMaximumWidth(360)
        lay.addWidget(wrap)

        view = QVBoxLayout()
        pics = QHBoxLayout()
        self.orig = QLabel("Original"); self.tandy = QLabel("Tandy")
        for w in (self.orig, self.tandy):
            w.setMinimumSize(480, 360)
            w.setAlignment(Qt.AlignmentFlag.AlignCenter)
            w.setStyleSheet("background: #111; color: #888")
        pics.addWidget(self.orig); pics.addWidget(self.tandy)
        view.addLayout(pics)
        self.show_pixels = QCheckBox("Show the raw 640x200 pixels, 2x (scroll).  This view looks squashed: "
                                     "Tandy pixels are about 2.4x taller than wide.  The 4:3 preview above shows the true shape.")
        self.show_pixels.setToolTip("Each Tandy pixel is drawn here as a square, so the picture looks flattened. "
                                    "On the Tandy's 4:3 monitor the 200 lines are stretched to full height.")
        view.addWidget(self.show_pixels)
        self.pixels = QLabel()
        scroll = QScrollArea(); scroll.setWidget(self.pixels); scroll.setWidgetResizable(False)
        scroll.setMinimumHeight(260)
        view.addWidget(scroll, 1)
        lay.addLayout(view, 1)

        self.engine.currentTextChanged.connect(self.fill_methods)
        for w in (self.mode, self.fit, self.engine, self.method):
            w.currentTextChanged.connect(self.changed)
        for w in (self.order, self.threshold, self.strength, self.brightness, self.contrast,
                  self.saturation, self.gamma, self.sharpen):
            w.valueChanged.connect(self.changed)
        for w in (self.serpentine, self.brown, self.show_pixels):
            w.toggled.connect(self.changed)
        self.debounce = QTimer(self); self.debounce.setSingleShot(True); self.debounce.setInterval(250)
        self.debounce.timeout.connect(self.render)

        self.load_settings(win.config.dither_settings())
        self.load_latest(quiet=True)

    # ------------------------------------------------------------ settings <-> widgets

    def fill_methods(self, engine: str):
        cur = self.method.currentText()
        self.method.blockSignals(True)
        self.method.clear()
        if engine == "hitherdither":
            self.method.addItems(D.HITHER_METHODS)
        elif engine == "didder":
            self.method.addItems(D.DIDDER_METHODS)
        elif engine == "pillow":
            self.method.addItems(["floyd-steinberg"])
        else:
            self.method.addItems(["nearest colour"])
        if self.method.findText(cur) >= 0:
            self.method.setCurrentText(cur)
        self.method.blockSignals(False)

    def settings(self) -> D.DitherSettings:
        s = self.win.config.dither_settings()
        return dataclasses.replace(
            s, mode=self.mode.currentText(), fit=self.fit.currentText(), engine=self.engine.currentText(),
            method=self.method.currentText(), order=self.order.value(), threshold=self.threshold.value(),
            strength=self.strength.value() / 100, serpentine=self.serpentine.isChecked(),
            brightness=self.brightness.value() / 100, contrast=self.contrast.value() / 100,
            saturation=self.saturation.value() / 100, gamma=self.gamma.value() / 100,
            sharpen=self.sharpen.value() / 100, brown=self.brown.isChecked())

    def load_settings(self, s: D.DitherSettings):
        self._loading = True
        self.mode.setCurrentText(s.mode); self.fit.setCurrentText(s.fit)
        self.engine.setCurrentText(s.engine); self.fill_methods(s.engine); self.method.setCurrentText(s.method)
        self.order.setValue(s.order); self.threshold.setValue(s.threshold)
        self.strength.setValue(round(s.strength * 100)); self.serpentine.setChecked(s.serpentine)
        self.brightness.setValue(round(s.brightness * 100)); self.contrast.setValue(round(s.contrast * 100))
        self.saturation.setValue(round(s.saturation * 100)); self.gamma.setValue(round(s.gamma * 100))
        self.sharpen.setValue(round(s.sharpen * 100)); self.brown.setChecked(s.brown)
        self._loading = False
        self.changed()

    # ------------------------------------------------------------ source

    def set_source(self, img: Image.Image, name: str):
        self.source = img.convert("RGB")
        self.source_name = name
        self.src_label.setText(name)
        self.orig.setPixmap(to_pixmap(fit_43(self.source, 480)))
        self.changed()

    def open_file(self):
        path, _ = QFileDialog.getOpenFileName(self, "Open image", HELPER_DIR, "Images (*.png *.jpg *.jpeg *.webp *.bmp)")
        if path:
            self.set_source(Image.open(path), os.path.basename(path))

    def load_latest(self, quiet=False):
        items = self.win.server.store.list()
        if items:
            m = items[0]
            self.set_source(self.win.server.store.original(m["id"]), f"{m['id']}  {m['title']}")
        elif not quiet:
            QMessageBox.information(self, "Dither Lab", "No generated images yet.")
        else:
            samples = os.path.join(HELPER_DIR, "samples")
            files = sorted(f for f in os.listdir(samples) if f.lower().endswith(".png")) if os.path.isdir(samples) else []
            if files:
                self.set_source(Image.open(os.path.join(samples, files[0])), files[0])

    # ------------------------------------------------------------ rendering

    def changed(self, *_):
        if not self._loading:
            self.debounce.start()

    def render(self):
        if self.source is None:
            return
        if self.busy:
            self.dirty = True
            return
        self.busy = True
        self.dirty = False
        s = self.settings()
        src = self.source
        self.timing.setText("working…")

        def work():
            t0 = time.perf_counter()
            idx = D.convert(src, s)
            return idx, s, time.perf_counter() - t0

        self._task = Task(work)             # keep it alive until its signal is delivered
        self._task.setAutoDelete(False)
        self._task.signals.done.connect(self.rendered)
        self.pool.start(self._task)

    def rendered(self, result, error):
        self.busy = False
        if error:
            self.timing.setText(f"Error: {error}")
        else:
            idx, s, dt = result
            self.tandy.setPixmap(to_pixmap(D.preview_4x3(idx, s, 480)))
            used = len(set(idx.flatten().tolist()))
            self.timing.setText(f"{dt * 1000:.0f} ms, {used} of 16 colours")
            if self.show_pixels.isChecked():
                raw = D.to_rgb(idx, s)
                self.pixels.setPixmap(to_pixmap(raw.resize((raw.width * 2, raw.height * 2), Image.Resampling.NEAREST)))
                self.pixels.adjustSize()
            else:
                self.pixels.clear()
        if self.dirty:
            self.render()

    # ------------------------------------------------------------ actions

    def store_preset(self, name):
        self.presets[name] = self.settings().to_dict()
        log.info("preset %s stored", name)

    def load_preset(self, name):
        if name in self.presets:
            self.load_settings(D.DitherSettings.from_dict(self.presets[name]))

    def save_default(self):
        self.win.config.data["dither"] = self.settings().to_dict()
        self.win.config.save()
        st = self.win.server.store
        for m in st.list():
            st.drop_tpi_cache(m["id"])
        log.info("dither settings saved as default; Tandy files will be rebuilt on next download")

    def send_to_tandy(self):
        if self.source is None:
            return
        title, ok = QInputDialog.getText(self, "Send to Tandy", "Title:", text=os.path.splitext(self.source_name)[0][:39])
        if not ok:
            return
        meta = self.win.server.store.add(self.source, title, self.settings(), title=title, source="lab")
        log.info("added %s (%s) to the gallery", meta["id"], meta["title"])
        self.win.gallery.refresh()


# ====================================================================== Gallery

class GalleryTab(QWidget):
    def __init__(self, win: "MainWindow"):
        super().__init__()
        self.win = win
        lay = QHBoxLayout(self)
        self.list = QListWidget()
        self.list.setViewMode(QListWidget.ViewMode.IconMode)
        self.list.setIconSize(QPixmap(160, 120).size())
        self.list.setResizeMode(QListWidget.ResizeMode.Adjust)
        self.list.setSpacing(8)
        self.list.currentItemChanged.connect(self.show_details)
        lay.addWidget(self.list, 3)
        side = QVBoxLayout()
        self.info = QPlainTextEdit(); self.info.setReadOnly(True)
        side.addWidget(self.info, 1)
        for text, fn in [("Refresh", self.refresh), ("Rename…", self.rename), ("Delete", self.delete),
                         ("Open in Dither Lab", self.to_lab), ("Rebuild Tandy files", self.rebuild)]:
            b = QPushButton(text); b.clicked.connect(fn); side.addWidget(b)
        wrap = QWidget(); wrap.setLayout(side); wrap.setMaximumWidth(340)
        lay.addWidget(wrap)
        self.refresh()

    def current_id(self) -> str | None:
        it = self.list.currentItem()
        return it.data(Qt.ItemDataRole.UserRole) if it else None

    def refresh(self):
        st = self.win.server.store
        keep = self.current_id()
        self.list.clear()
        for m in st.list():
            try:
                info = tpi.parse(st.tpi_bytes(m["id"]))
                th = D.to_rgb(D.unpack(info.thumb, info.thumb_w, info.thumb_h), self.win.config.dither_settings())
                icon = QIcon(to_pixmap(th.resize((160, 120), Image.Resampling.NEAREST)))
            except Exception:
                icon = QIcon()
            it = QListWidgetItem(icon, f"{m['id']}\n{m['title'][:24]}")
            it.setData(Qt.ItemDataRole.UserRole, m["id"])
            self.list.addItem(it)
            if m["id"] == keep:
                self.list.setCurrentItem(it)

    def show_details(self, *_):
        id_ = self.current_id()
        if not id_:
            self.info.clear()
            return
        m = self.win.server.store.meta(id_)
        self.info.setPlainText(
            f"ID: {m['id']}\nTitle: {m['title']}\nCreated: {m['created']}\nMode: {m.get('mode')}\n"
            f"Seed: {m.get('seed')}\nSource: {m.get('source')}\n\nPrompt:\n{m['prompt']}\n\n"
            f"Original prompt:\n{m.get('original_prompt', '')}")

    def rename(self):
        id_ = self.current_id()
        if not id_:
            return
        m = self.win.server.store.meta(id_)
        title, ok = QInputDialog.getText(self, "Rename", "Title (39 characters max):", text=m["title"])
        if ok and title.strip():
            self.win.server.store.rename(id_, title)
            self.refresh()

    def delete(self):
        id_ = self.current_id()
        if id_ and QMessageBox.question(self, "Delete", f"Delete {id_} from MindServer?") == QMessageBox.StandardButton.Yes:
            self.win.server.store.delete(id_)
            self.refresh()

    def to_lab(self):
        id_ = self.current_id()
        if id_:
            m = self.win.server.store.meta(id_)
            self.win.lab.set_source(self.win.server.store.original(id_), f"{id_}  {m['title']}")
            self.win.tabs.setCurrentWidget(self.win.lab)

    def rebuild(self):
        id_ = self.current_id()
        if id_:
            st = self.win.server.store
            st.drop_tpi_cache(id_)
            s = self.win.config.dither_settings()
            s.mode = st.meta(id_).get("mode", s.mode)
            st.build_tpi(id_, s)
            self.refresh()


# ====================================================================== AI (Qwen)

class AITab(QWidget):
    """Qwen settings, the three prompts, and a test chat that shows exactly what the Tandy receives."""

    chat_line = Signal(str)

    def __init__(self, win: "MainWindow"):
        super().__init__()
        self.win = win
        q = win.config["qwen"]
        lay = QHBoxLayout(self)
        left = QVBoxLayout()

        box = QGroupBox("NInfer / Qwen")
        f = QFormLayout(box)
        self.url = QLineEdit(q["url"])
        self.model = QLineEdit(q["model"]); self.model.setPlaceholderText("blank = first model the server lists")
        f.addRow("URL", self.url)
        f.addRow("Model", self.model)
        self.efforts = {}
        for key, label in (("effort_chat", "Thinking: chat"), ("effort_enhance", "Thinking: prompt enhancement"),
                           ("effort_vision", "Thinking: questions about pictures")):
            c = QComboBox(); c.addItems(["none", "low", "medium", "xhigh"]); c.setCurrentText(q[key])
            self.efforts[key] = c
            f.addRow(label, c)
        self.temperature = QDoubleSpinBox(); self.temperature.setRange(0, 2); self.temperature.setSingleStep(0.05)
        self.temperature.setSpecialValueText("model default"); self.temperature.setValue(q["temperature"])
        self.max_tokens = QSpinBox(); self.max_tokens.setRange(300, 32000); self.max_tokens.setValue(q["max_tokens"])
        self.draw_enhance = QCheckBox("Also enhance pictures drawn from chat (slower)")
        self.draw_enhance.setChecked(bool(q.get("draw_enhance", False)))
        f.addRow("Temperature", self.temperature)
        f.addRow("Max tokens", self.max_tokens)
        f.addRow(self.draw_enhance)
        b = QPushButton("Save settings"); b.clicked.connect(self.save)
        f.addRow(b)
        left.addWidget(box)

        pb = QGroupBox("Prompts (saved to helper\\prompts\\, used from the next request on)")
        pl = QVBoxLayout(pb)
        row = QHBoxLayout()
        self.which = QComboBox()
        self.which.addItems(["chat", "enhance", "vision"])
        self.which.currentTextChanged.connect(self.load_prompt)
        b_save = QPushButton("Save prompt"); b_save.clicked.connect(self.save_prompt)
        row.addWidget(QLabel("Prompt:")); row.addWidget(self.which); row.addStretch(); row.addWidget(b_save)
        pl.addLayout(row)
        self.prompt = QPlainTextEdit()
        pl.addWidget(self.prompt)
        hint = QLabel("chat: {date} = today.   vision: {image_id}, {title}, {prompt} = the attached picture.")
        hint.setStyleSheet("color: #888")
        pl.addWidget(hint)
        left.addWidget(pb, 1)
        lay.addLayout(left, 1)

        tb = QGroupBox("Test chat (what the Tandy receives)")
        tl = QVBoxLayout(tb)
        self.transcript = QPlainTextEdit(); self.transcript.setReadOnly(True)
        self.transcript.setStyleSheet("font-family: Consolas, monospace; background: #0000aa; color: #ffffff")
        tl.addWidget(self.transcript, 1)
        self.raw = QCheckBox("Show protocol lines")
        tl.addWidget(self.raw)
        row2 = QHBoxLayout()
        self.msg = QLineEdit(); self.msg.setPlaceholderText("Type a message (try: draw me a castle)")
        self.msg.returnPressed.connect(self.send)
        b_send = QPushButton("Send"); b_send.clicked.connect(self.send)
        b_new = QPushButton("New chat"); b_new.clicked.connect(self.new_chat)
        row2.addWidget(self.msg, 1); row2.addWidget(b_send); row2.addWidget(b_new)
        tl.addLayout(row2)
        lay.addWidget(tb, 1)

        self.chat_id = None
        self.chat_line.connect(self.show_line)
        self.load_prompt("chat")

    # ------------------------------------------------------------ settings and prompts

    def save(self):
        q = self.win.config["qwen"]
        q.update(url=self.url.text().strip(), model=self.model.text().strip(),
                 temperature=round(self.temperature.value(), 2), max_tokens=self.max_tokens.value(),
                 draw_enhance=self.draw_enhance.isChecked(),
                 **{k: c.currentText() for k, c in self.efforts.items()})
        self.win.config.save()
        log.info("Qwen settings saved")

    def load_prompt(self, name):
        from .chat import load_prompt
        self.prompt.setPlainText(load_prompt(name))

    def save_prompt(self):
        from .chat import save_prompt
        save_prompt(self.which.currentText(), self.prompt.toPlainText())
        log.info("prompt '%s' saved", self.which.currentText())

    # ------------------------------------------------------------ test chat

    def new_chat(self):
        self.chat_id = None
        self.transcript.clear()

    def send(self):
        text = self.msg.text().strip()
        if not text:
            return
        self.msg.clear()
        self.transcript.appendPlainText(f"\nYou: {text}\nDeskMind: ")
        engine, chat_id = self.win.server.chat, self.chat_id

        def work():
            return engine.reply(chat_id, text, None, self.chat_line.emit)

        self._task = Task(work)             # keep it alive until its signal is delivered
        self._task.setAutoDelete(False)
        self._task.signals.done.connect(self.replied)
        QThreadPool.globalInstance().start(self._task)

    def replied(self, chat, err):
        if err:
            self.transcript.appendPlainText(f"[error: {err}]")
        elif chat:
            self.chat_id = chat["id"]

    def show_line(self, line: str):
        if self.raw.isChecked():
            self.transcript.appendPlainText(line)
            return
        cur = self.transcript.textCursor()
        cur.movePosition(cur.MoveOperation.End)
        if line.startswith("T "):
            cur.insertText(line[2:])
        elif line == "N":
            cur.insertText("\n")
        elif line.startswith("I "):
            cur.insertText(f"\n[picture {line[2:]} - see the Gallery]\n")
            self.win.gallery.refresh()
        elif line.startswith("E "):
            cur.insertText(f"\n[error: {line[2:]}]\n")
        elif line.startswith("S drawing"):
            pass
        self.transcript.setTextCursor(cur)
        self.transcript.ensureCursorVisible()


# ====================================================================== Tandy

class TandyTab(QWidget):
    def __init__(self, win: "MainWindow"):
        super().__init__()
        self.win = win
        lay = QVBoxLayout(self)
        box = QGroupBox("Connection (port changes need a restart)")
        f = QFormLayout(box)
        s = win.config["server"]
        self.port = QSpinBox(); self.port.setRange(1024, 65535); self.port.setValue(int(s["port"]))
        self.token = QLineEdit(s["token"]); self.token.setPlaceholderText("blank = no token")
        self.mode = QComboBox(); self.mode.addItems(["640", "320"]); self.mode.setCurrentText(win.config["dither"]["mode"])
        f.addRow("Port", self.port)
        f.addRow("Shared token", self.token)
        f.addRow("Default video mode", self.mode)
        lay.addWidget(box)

        pb = QGroupBox("Palette: how the 16 Tandy colours look on your monitor (used for dithering and previews)")
        pg = QGridLayout(pb)
        self.swatches = []
        for i in range(16):
            b = QPushButton(str(i))
            b.setFixedSize(56, 36)
            b.clicked.connect(lambda _=False, n=i: self.pick(n))
            pg.addWidget(b, i // 8, i % 8)
            self.swatches.append(b)
        reset = QPushButton("Reset to standard RGBI")
        reset.clicked.connect(self.reset_palette)
        pg.addWidget(reset, 2, 0, 1, 3)
        lay.addWidget(pb)
        self.palette = list(win.config["dither"]["palette"])
        self.paint()

        fb = QGroupBox("DOS builds served to the Tandy at /files/<NAME>")
        fl = QVBoxLayout(fb)
        out = os.path.join(PROJECT_DIR, "dos", "out")
        names = sorted(os.listdir(out)) if os.path.isdir(out) else []
        fl.addWidget(QLabel("  ".join(names) or "(none built yet)"))
        lay.addWidget(fb)

        b = QPushButton("Save")
        b.clicked.connect(self.save)
        lay.addWidget(b)
        lay.addStretch()

    def paint(self):
        for i, b in enumerate(self.swatches):
            c = self.palette[i]
            fg = "#000" if (((c >> 16) & 255) * 3 + ((c >> 8) & 255) * 6 + (c & 255)) > 1300 else "#fff"
            b.setStyleSheet(f"background: #{c:06X}; color: {fg}")

    def pick(self, n):
        c = QColorDialog.getColor(QColor(f"#{self.palette[n]:06X}"), self, f"Colour {n}")
        if c.isValid():
            self.palette[n] = int(c.name()[1:], 16)
            self.paint()

    def reset_palette(self):
        self.palette = list(D.TANDY_RGB)
        self.paint()

    def save(self):
        s = self.win.config["server"]
        s.update(port=self.port.value(), token=self.token.text().strip())
        self.win.config.data["dither"]["mode"] = self.mode.currentText()
        self.win.config.data["dither"]["palette"] = self.palette
        self.win.config.save()
        log.info("Tandy settings saved")


# ====================================================================== window

class MainWindow(QMainWindow):
    def __init__(self, config: Config, server: MindServer):
        super().__init__()
        self.config = config
        self.server = server
        self.setWindowTitle(f"MindServer {VERSION} - DeskMind helper")
        self.resize(1320, 820)
        self.bridge = Bridge()

        self.tabs = QTabWidget()
        self.services = ServicesTab(self)
        self.generation = GenerationTab(self)
        self.lab = DitherLabTab(self)
        self.gallery = GalleryTab(self)
        self.tabs.addTab(self.services, "Services")
        self.tabs.addTab(self.generation, "Generation")
        self.tabs.addTab(self.lab, "Dither Lab")
        self.tabs.addTab(self.gallery, "Gallery")
        self.ai = AITab(self)
        self.tabs.addTab(self.ai, "AI")
        self.tabs.addTab(TandyTab(self), "Tandy")
        self.setCentralWidget(self.tabs)
        s = config["server"]
        self.statusBar().showMessage(f"Listening on port {s['port']}  -  data in {os.path.join(HELPER_DIR, 'data')}")

        self.bridge.log_line.connect(self.services.logview.appendPlainText)
        self.bridge.status.connect(self.services.set_status)
        self.bridge.job.connect(self.on_job)
        handler = QtLogHandler(self.bridge)
        logging.getLogger().addHandler(handler)
        self._handler = handler
        server.services.on_change = self.bridge.status.emit
        server.jobs.listeners.append(self.bridge.job.emit)

    def on_job(self, job):
        self.generation.on_job(job)
        if job.status == "done":
            self.gallery.refresh()

    def closeEvent(self, ev):
        logging.getLogger().removeHandler(self._handler)
        self.server.stop()
        super().closeEvent(ev)


def run_gui(config: Config, server: MindServer) -> int:
    app = QApplication.instance() or QApplication([])
    app.setApplicationName("MindServer")
    win = MainWindow(config, server)
    win.show()
    return app.exec()
