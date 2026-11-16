#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Sample out-of-process stdio JSON plugin for WINOME."""

import json
import sys
import threading
import time

state_lock = threading.Lock()
turbo_mode = False
stop_event = threading.Event()


def emit(obj):
    sys.stdout.write(json.dumps(obj, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def stdin_reader():
    global turbo_mode
    for raw in sys.stdin:
        line = raw.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except Exception:
            continue

        mtype = msg.get("type")
        if mtype == "ping":
            emit({"type": "pong"})
        elif mtype == "shutdown":
            stop_event.set()
            break
        elif mtype == "event":
            ev = msg.get("event")
            if ev == "topbar.click":
                emit({
                    "type": "notify",
                    "title": "System Monitor",
                    "body": "Sample plugin TopBar button clicked!",
                })
            elif ev == "quicksettings.toggle":
                with state_lock:
                    turbo_mode = bool(msg.get("active", False))
                emit({
                    "type": "quicksettings.upsert",
                    "slot": "sample",
                    "icon": "utilities-system-monitor-symbolic",
                    "title": "Sample Monitor",
                    "subtitle": "Fast Poll (1s)" if turbo_mode else "Normal Poll (3s)",
                    "active": turbo_mode,
                })
            elif ev == "desktop.changed":
                sys.stderr.write(
                    f"desktop changed: active={msg.get('active')} count={msg.get('count')}\n"
                )
                sys.stderr.flush()


def main():
    threading.Thread(target=stdin_reader, daemon=True).start()
    emit({"type": "ready"})
    emit({
        "type": "quicksettings.upsert",
        "slot": "sample",
        "icon": "utilities-system-monitor-symbolic",
        "title": "Sample Monitor",
        "subtitle": "Normal Poll (3s)",
        "active": False,
    })

    ticks = 0
    while not stop_event.is_set():
        ticks += 1
        emit({
            "type": "topbar.upsert",
            "slot": "heartbeat",
            "icon": "utilities-system-monitor-symbolic",
            "text": f"Plug #{ticks}",
            "tooltip": "Sample out-of-process stdio JSON plugin",
        })
        with state_lock:
            delay = 1.0 if turbo_mode else 3.0
        stop_event.wait(delay)


if __name__ == "__main__":
    main()
