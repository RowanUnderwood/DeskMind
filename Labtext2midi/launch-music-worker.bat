@echo off
rem DeskMind's music worker: MIDI-GPT on the RTX 3090, http://127.0.0.1:8287 (MindServer starts it).
title DeskMind music worker
cd /d "%~dp0"
"%~dp0MIDI-GPT\.venv\Scripts\python.exe" "%~dp0worker\music_worker.py" %*
