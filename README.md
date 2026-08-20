# Zerone Performance Management System
### The MacBook Pro 2011 Project

I'm Daily driving this old 2011 13" MacBook Pro as my main machine, and This management system
is written to manage it's performance by reducing the tick rate of non-focused opened windows.
"Tickle Subsystem" has three options

- High
- Moderate
- Nominal
- None

That defines its aggressiveness.

## Build

```bash
# Build CLI / TUI
make

# Build with GTK3 GUI support
make gui

# Clean
make clean
```

## CLI Usage

```bash
./zrn_perfd [OPTIONS]
```

### Options

| Flag | Description |
|---|---|
| `-d`, `--daemon` | Run headless (no TUI, logs to stdout/journald). |
| `-g`, `--gui` | Launch GTK3 graphical interface. |
| `-m <mode>`, `--mode=<mode>` | Set mode on startup (`none`, `nominal`, `moderate`, `high`). |
| `-p`, `--pairing` | Enable frequent-switch pair exemption logic. |
| `-x`, `--allow-experimental-features` | Enable neural-network pre-warming & switch prediction data usage. |
| `-h`, `--help` | Display usage help. |

> **Note:** If a daemon instance is already active (via systemd or background process), launching `./zrn_perfd` or `./zrn_perfd --gui` automatically opens the GTK3 GUI in **Monitor Mode** without sending duplicate signals.

---

## TUI Keybindings

| Key | Action |
|---|---|
| `m` / `M` | Cycle performance mode (`None` &rarr; `Nominal` &rarr; `Moderate` &rarr; `High`). |
| `e` / `E` | Toggle permanent exemption for the selected/focused process. |
| `l` / `L` | Toggle the persistent exemptions list view. |
| `q` / `Q` | Quit and unthrottle all processes. |

---

## GUI Controls

- **Mode Dropdown**: Switch between `None`, `Nominal`, `Moderate`, and `High` in real-time.
- **Processes Tab**: Displays PID, process name, state, unfocused duration, audio status, GPU status, and exemption state. Includes `Toggle Exempt for Selected`.
- **Exemptions Tab**: Lists all persistent exemptions from disk. Includes `Remove Selected from Exemptions`.

---

## Configuration & Files

| Path | Purpose |
|---|---|
| `~/.zrnperformanceprofile` | Current performance mode string (`None`, `Nominal`, `Moderate`, `High`). |
| `~/.zrnperformanceexempt` | Permanent exemption list (one `comm` name per line). |
| `~/.zrn_switch_data.csv` | Window switch history dataset for ML pre-warming. |
| `~/.zrn_model_weights.bin` | Trained neural-network weight parameters. |
| `~/.zrn_perf.lock` | Exclusive instance lockfile. |

---

## systemd User Service (`zrn-mbp-tickle.service`)

Create `~/.config/systemd/user/zrn-mbp-tickle.service`:

```ini
[Unit]
Description=Zrn MBP Tickle Background Daemon
After=graphical-session.target

[Service]
Type=simple
ExecStart=/home/zerone/Applications/ZrnPerformanceMgmnt/zrn_perfd --daemon --allow-experimental-features
Restart=on-failure
RestartSec=5
Environment="DISPLAY=:0"

[Install]
WantedBy=default.target
```

### Service Commands

```bash
# Reload unit files
systemctl --user daemon-reload

# Enable on login and start immediately
systemctl --user enable --now zrn-mbp-tickle.service

# Check status
systemctl --user status zrn-mbp-tickle.service

# View live logs
journalctl --user -fu zrn-mbp-tickle.service
```
