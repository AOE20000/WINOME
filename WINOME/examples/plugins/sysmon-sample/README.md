# WINOME Out-of-Process Plugin Example (`sysmon-sample`)

Each plugin runs inside an isolated Win32 Job Object child process and exchanges
line-delimited JSON messages (`<= 64 KB` per line, rate-limited to `30 msg/s`)
over `stdin` / `stdout` (`stderr` lines are forwarded directly to Looking Glass
under tag `plugin:<id>`).

## Install

Copy this folder (`sysmon-sample/`) to:

```text
%LOCALAPPDATA%\WINOME\plugins\sysmon-sample\
```

Ensure `python.exe` is on `PATH`, then open **Settings -> Behavior -> Developer Mode -> Open Console** (or run `winome-ctl.exe plugin reload`).

## JSON Line Protocol (`protocol = 1`)

### Host -> Plugin (`stdin`)
- `{"type":"hello","protocol":1,"plugin_id":"...","developer_mode":bool,"capabilities":[...]}`
- `{"type":"event","event":"topbar.click","slot":"..."}`
- `{"type":"event","event":"quicksettings.toggle","slot":"...","active":bool}`
- `{"type":"event","event":"desktop.changed","count":N,"active":I}` (requires capability `desktop.read`)
- `{"type":"ping"}`
- `{"type":"shutdown"}`

### Plugin -> Host (`stdout`)
- `{"type":"ready"}`
- `{"type":"pong"}`
- `{"type":"log","level":"info","message":"..."}`
- `{"type":"topbar.upsert","slot":"...","icon":"icon-symbolic","text":"...","tooltip":"..."}` *(requires `topbar`)*
- `{"type":"topbar.remove","slot":"..."}` *(requires `topbar`)*
- `{"type":"quicksettings.upsert","slot":"...","icon":"icon-symbolic","title":"...","subtitle":"...","active":bool}` *(requires `quicksettings`)*
- `{"type":"quicksettings.remove","slot":"..."}` *(requires `quicksettings`)*
- `{"type":"notify","title":"...","body":"...","app_name":"..."}` *(requires `notify`)*

## Circuit Breaker & Safe Mode
- `3` unexpected exits within `60s` automatically trip the plugin (`Tripped`) and clear all of its UI widgets.
- Recover via **Looking Glass -> Plugins & Flags -> Reset**, or `winome-ctl.exe plugin reset <id>`.
- Launching `winome.exe --safe-mode` skips starting all plugins.
