"""Image generation queue: one worker, one ComfyUI job at a time."""

from __future__ import annotations

import itertools
import logging
import queue
import threading
import time
from dataclasses import dataclass, field

from .comfy import ComfyClient
from .config import Config
from .store import Store

log = logging.getLogger("mindserver.jobs")


@dataclass
class Job:
    id: int
    prompt: str
    mode: str
    seed: int = -1
    title: str | None = None
    original_prompt: str = ""
    enhance: bool = False           # rewrite the prompt with Qwen before generating
    status: str = "queued"          # queued | running | done | error
    progress: float = 0.0           # 0..1
    text: str = "waiting"
    image_id: str = ""
    error: str = ""
    created: float = field(default_factory=time.time)
    finished: float = 0.0

    def line(self) -> str:
        """One status line in the DeskMind protocol."""
        if self.status == "done":
            return f"I {self.image_id} {self.title or ''}".rstrip()
        if self.status == "error":
            return f"E {self.error}"
        return f"S {self.status} {int(self.progress * 100)} {self.text}"


class JobQueue:
    def __init__(self, config: Config, store: Store):
        self.config = config
        self.store = store
        self.jobs: dict[int, Job] = {}
        self._ids = itertools.count(1)
        self._q: queue.Queue[Job] = queue.Queue()
        self._cond = threading.Condition()
        self.listeners: list = []           # callables(job), e.g. the GUI
        self.prepare = None                 # optional callable(job) before generating (prompt enhancement)
        threading.Thread(target=self._worker, name="jobs", daemon=True).start()

    # ------------------------------------------------------------ API

    def submit(self, prompt: str, mode: str | None = None, seed: int = -1,
               title: str | None = None, original_prompt: str = "", enhance: bool = False) -> Job:
        job = Job(next(self._ids), prompt.strip(), mode or self.config["dither"]["mode"], seed, title,
                  original_prompt or prompt.strip(), enhance)
        self.jobs[job.id] = job
        self._q.put(job)
        self._notify(job)
        log.info("job %d queued: %s", job.id, job.prompt[:80])
        return job

    def get(self, job_id: int) -> Job | None:
        return self.jobs.get(job_id)

    def wait_change(self, job: Job, last_line: str, timeout: float = 1.0) -> str:
        """Block until the job's status line differs from `last_line` (or timeout)."""
        with self._cond:
            self._cond.wait_for(lambda: job.line() != last_line, timeout)
        return job.line()

    # ------------------------------------------------------------ worker

    def _notify(self, job: Job) -> None:
        with self._cond:
            self._cond.notify_all()
        for fn in list(self.listeners):
            try:
                fn(job)
            except Exception:
                log.exception("job listener failed")

    def _update(self, job: Job, **kw) -> None:
        for k, v in kw.items():
            setattr(job, k, v)
        self._notify(job)

    def _worker(self) -> None:
        while True:
            job = self._q.get()
            try:
                self._run(job)
            except Exception as e:
                log.exception("job %d failed", job.id)
                self._update(job, status="error", error=str(e)[:150], finished=time.time())

    def _run(self, job: Job) -> None:
        cfg = self.config
        self._update(job, status="running", text="starting")
        if self.prepare:
            self.prepare(job)
        c = cfg["comfy"]
        client = ComfyClient(c["url"], c["workflow"])
        if not client.alive():
            raise RuntimeError("ComfyUI is not running")
        wf, seed = client.build(job.prompt, seed=job.seed if job.seed >= 0 else None, steps=c["steps"],
                                shortside=c["shortside"], lora_on=c["lora_on"],
                                lora_strength=c["lora_strength"])
        job.seed = seed
        steps_weight = 0.9

        def progress(frac, text):
            if frac is not None:
                self._update(job, progress=frac * steps_weight, text=text)
            else:
                self._update(job, text=text)

        self._update(job, text="generating")
        image = client.run(wf, progress=progress)
        self._update(job, progress=0.92, text="dithering")
        settings = cfg.dither_settings()
        settings.mode = job.mode
        meta = self.store.add(image, job.prompt, settings, title=job.title, seed=seed,
                              original_prompt=job.original_prompt)
        self._update(job, status="done", progress=1.0, text="done", image_id=meta["id"],
                     title=meta["title"], finished=time.time())
        log.info("job %d done: %s (%s)", job.id, meta["id"], meta["title"])
