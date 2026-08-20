# Zerone Performance Management System (`zrn_perfd`)
### The MacBook Pro 2011 Project (Openbox / LXDE / Lightweight Linux)

A lightweight, hardware-aware process tickler, CPU governor, and predictive performance management daemon designed to optimize Linux responsiveness, battery longevity, and thermal stability on resource-constrained hardware (specifically tailored for the 13" MacBook Pro 2011 with Sandy Bridge Intel Core i5 and Intel HD3000 Graphics).

---

## Architecture & Subsystems

```
                                  +---------------------------------------+
                                  |         X11 Window Manager (EWMH)     |
                                  |     _NET_ACTIVE_WINDOW / _NET_WM_PID  |
                                  +-------------------+-------------------+
                                                      |
                                                      v
+---------------------------------------------------------------------------------------------------------+
|                                        zrn_perfd Core Daemon                                            |
|                                                                                                         |
|  +---------------------------+  +--------------------------+  +--------------------------------------+  |
|  |     Tickle Subsystem      |  |   Per-App CPU Limits     |  |   Dynamic Boost & RapidBoost         |  |
|  |  - High / Mod / Nominal   |  |  - SIGSTOP/CONT slicing  |  |  - AC: 2.3 GHz -> 2.6 GHz Turbo      |  |
|  |  - SIGSTOP / SIGCONT      |  |  - Persistent limits     |  |  - Battery: 900 MHz -> 1.15 GHz (50%)|  |
|  |  - Pulse tickle intervals |  |  - ~/.zrnperformancelim  |  |  - Display / Kbd Backlight Memory    |  |
|  +-------------+-------------+  +--------------------------+  +------------------+-------------------+  |
|                |                                                                 |                      |
|                v                                                                 v                      |
|  +---------------------------+                                +--------------------------------------+  |
|  |   Exemption Monitors      |                                |   Direct Hardware Power Management   |  |
|  |  - Audio: PulseAudio Sink |                                |  - Native sysfs AC/Bat Detection     |  |
|  |  - GPU: DRM Render Nodes  |                                |  - HDD Spindown (30s idle timeout)   |  |
|  |  - Frequent-Switch Pairs  |                                |  - cpufreq / scaling_max_freq sync   |  |
|  +---------------------------+                                +--------------------------------------+  |
|                                                                                                         |
|  +---------------------------------------------------------------------------------------------------+  |
|  |                Next Window Prediction Engine (OpenGL 3.3 GPU + Online RL)                         |  |
|  |                                                                                                   |  |
|  |  [Input Vector (16D)]  -->  [Dense 1 (32 ReLU)]  -->  [Dense 2 (16 ReLU)]  -->  [Output (64 Softmax)] |  |
|  |  - Comm hash, Time,         - Offscreen GLX Pbuffer      - GLSL 3.30 Fragment   - Probabilities   |  |
|  |    Switch Rate, History       on Intel HD3000              Shader Matrix Mult     Top-5 Ranking   |  |
|  |                                                                                                   |  |
|  |  [Reinforcement Learning & Online SGD]: Evaluates actual switch, computes Cross-Entropy Loss,   |  |
|  |   backpropagates gradients (dz3 = p - 1), updates weights online, syncs GPU textures.             |  |
|  |                                                                                                   |  |
|  |  [Pre-warming Action]: Top predicted candidate window is automatically pre-warmed (100ms pulse). |  |
|  +---------------------------------------------------------------------------------------------------+  |
+---------------------------------------------------------------------------------------------------------+
                                                      |
                      +-------------------------------+-------------------------------+
                      |                               |                               |
                      v                               v                               v
             +-----------------+             +-----------------+             +-----------------+
             |   CLI Tool      |             |   Ncurses TUI   |             |    GTK3 GUI     |
             | --predict       |             | Interactive     |             | 4 Tabs + Status |
             | --set-limit     |             | Dashboard       |             | Monitor Mode    |
             +-----------------+             +-----------------+             +-----------------+
```

---

## Key Features

### 1. Adaptive Tickle Engine
Reduces CPU load and thermals from background and unfocused windows by alternating `SIGSTOP` and `SIGCONT` signals:
- **Modes**:
  - `High`: 5s grace period, 2000ms pause / 20ms active.
  - `Moderate`: 15s grace period, 1000ms pause / 50ms active.
  - `Nominal`: 30s grace period, 500ms pause / 100ms active.
  - `None`: Tickling disabled.
- **Smart Automatic Exemptions**:
  - **Focused Window**: Always allocated 100% full CPU.
  - **Audio Streams**: Active PulseAudio playback applications (e.g. Spotify, YouTube, media players) are automatically detected via asynchronous pulse monitoring and never throttled.
  - **GPU Acceleration**: Processes actively utilizing `/dev/dri/renderD128` or `/dev/dri/card0` (e.g. WebGL, video decoders) are exempt.
  - **Frequent Switch Pairs**: Applications frequently switched back-and-forth are dynamically paired and exempted.
  - **Permanent Exemption List**: User-selected processes saved persistently in `~/.zrnperformanceexempt`.

### 2. Per-Application CPU Limits
Enforce hard CPU utilization ceilings on stubborn applications without killing them:
- Granular percentage limits (e.g. `30%` of 4 cores, `100%` of 1 core).
- Saved persistently across restarts in `~/.zrnperformancelimits`.
- Configurable via CLI (`--set-limit`, `--remove-limit`) and GUI.

### 3. Dynamic Boost & RapidBoost
- **AC Operation**: Nominal max frequency is 2.3 GHz. When the focused window saturates CPU ($\ge 95\%$), the daemon ramps cores up to **2.6 GHz Turbo**.
- **Battery Operation**: Nominal max frequency is capped at 900 MHz (powersave). When the focused window saturates CPU ($\ge 70\%$), RapidBoost engages:
  - CPU is boosted to **1.15 GHz** (50% of hardware max).
  - Keyboard backlight is disabled (turned OFF) to conserve power.
  - **Display Brightness Memorization**: Memorizes user-set display brightness. Dims display to 25% *only* if current brightness is higher than 25% (if already lower, leaves brightness untouched).
  - On deactivation, CPU clocks normalize and the exact memorized brightness is restored.

### 4. Direct Hardware Power Management
- Direct sysfs AC/Battery detection without dependencies on TLP.
- Automatic mechanical HDD spindown (`/dev/sda`) on battery after 30 seconds of I/O inactivity via ATA standby commands.

---

## Machine Learning & GPU Inference Engine

The Next Window Prediction model anticipates which window you will switch to next and **pre-warms** it so that it is instantly responsive the moment you focus it.

### Neural Network Architecture

| Layer | Type | Input Dim | Output Dim | Activation / Details |
|---|---|---|---|---|
| **Input** | Feature Vector | - | 16 | $x_0$: current comm hash, $x_1$: time of day $[0, 1]$, $x_2$: 60s switch rate, $x_{3..15}$: switch history hashes |
| **Layer 1** | Dense Hidden | 16 | 32 | ReLU ($\max(0, z)$), Weights: $W_1 [32 \times 16]$, Biases: $b_1 [32]$ |
| **Layer 2** | Dense Hidden | 32 | 16 | ReLU ($\max(0, z)$), Weights: $W_2 [16 \times 32]$, Biases: $b_2 [16]$ |
| **Layer 3** | Dense Output | 16 | 64 | Softmax ($p_i = \frac{e^{z_i - \max(z)}}{\sum e^{z_j - \max(z)}}$), Weights: $W_3 [64 \times 16]$, Biases: $b_3 [64]$ |

### GPU Acceleration (OpenGL 3.3 Core Profile)
- Runs offscreen tensor evaluations on the **Intel HD3000 GPU** using GLX Pbuffers and GLSL 3.30 fragment shaders rendering to `GL_R32F` floating-point texture targets.
- Falls back to a vector CPU forward pass if OpenGL initialization is unavailable.
- Inference triggers **only once upon a window focus change** to maintain zero CPU/GPU overhead between switches.

### Reinforcement Learning (Online Self-Correction)
- **Online Reward & Loss Calculation**:
  - When focus switches, the model compares its previous prediction against the actual window focused.
  - Reward: $+1.0$ (Correct) / Penalty: $-1.0$ (Incorrect).
  - Cross-Entropy Step Loss: $\mathcal{L} = -\ln(p_{\text{actual}})$.
- **Online Backpropagation**:
  - Computes error gradients $dz_3 = p - \mathbf{1}_{\text{actual}}$, $dz_2 = (W_3^T dz_3) \odot \mathbf{1}_{a_2 > 0}$, and $dz_1 = (W_2^T dz_2) \odot \mathbf{1}_{a_1 > 0}$.
  - Updates weights $W_1, b_1, W_2, b_2, W_3, b_3$ via Stochastic Gradient Descent ($\eta = 0.02$).
  - Re-uploads updated weight matrices directly into OpenGL texture memory and periodically syncs to `~/.zrn_model_weights.bin`.

---

## Build & Installation

### Prerequisites (Debian / Ubuntu / Arch)
```bash
# Required libraries: X11, Ncurses, PulseAudio, OpenGL, GTK3
sudo apt install build-essential libx11-dev libncurses-dev libpulse-dev libgl1-mesa-dev libgtk-3-dev
```

### Compiling
```bash
# Build CLI and TUI
make

# Build with GTK3 GUI support
make gui

# Install binary to ~/bin/zrn_perfd
make install

# Train initial model weights on collected switch dataset
make train
```

---

## CLI Usage & Options

```bash
zrn_perfd [OPTIONS]
```

| Flag | Description |
|---|---|
| `-d`, `--daemon` | Run headless daemon (logs to journal / stdout). |
| `-g`, `--gui` | Launch GTK3 graphical monitor interface. |
| `-x`, `--allow-experimental-features` | Enable GPU Neural Net next-window prediction & online RL. |
| `-s`, `--predict`, `--status` | Display current active window, top predicted window probabilities, and RL accuracy statistics via CLI. |
| `-m <mode>`, `--mode=<mode>` | Set power mode on startup (`none`, `nominal`, `moderate`, `high`). |
| `-l comm:pct`, `--set-limit comm:pct` | Set persistent CPU limit percentage for an application (e.g. `firefox:30`). |
| `-r comm`, `--remove-limit comm` | Remove CPU limit for an application. |
| `-p`, `--pairing` | Enable frequent-switch pair exemption logic. |
| `-h`, `--help` | Display usage help. |

### CLI Examples

```bash
# View live window prediction probabilities & RL accuracy
zrn_perfd --predict

# Set persistent 30% CPU limit on Firefox
zrn_perfd --set-limit firefox:30

# Remove CPU limit
zrn_perfd --remove-limit firefox
```

---

## User Interfaces

### 1. Terminal User Interface (TUI)
Run `zrn_perfd` or `make run` in a terminal:
- **Keys**:
  - `m` / `M`: Cycle performance mode (`None` &rarr; `Nominal` &rarr; `Moderate` &rarr; `High`).
  - `e` / `E`: Toggle permanent exemption for selected/focused process.
  - `l` / `L`: Toggle persistent exemptions list.
  - `q` / `Q`: Quit and unthrottle all processes.
- Displays process CPU usage, audio status, GPU status, RapidBoost state, and real-time **Next Window Probabilities** with ASCII probability bars and RL metrics.

### 2. Graphical User Interface (GUI)
Run `zrn_perfd --gui` or `make run-gui`:
- **Processes Tab**: Real-time process table with CPU usage, throttled state, audio exemption, GPU status, and CPU limits.
- **Window Predictor (RL) Tab**: Displays ranked next-window candidates, probability percentages, pre-warm action state, and live RL feedback / online accuracy.
- **Exemptions Tab**: Manage permanent application exemptions.
- **CPU Limits Tab**: Add and remove custom CPU limits per application.
- **Status Bar**: Live indicator of daemon status, RapidBoost state, and predicted target window.

---

## systemd Service Setup (`zrn-mbp-tickle.service`)

The daemon runs as a system service located at `/etc/systemd/system/zrn-mbp-tickle.service`:

```ini
[Unit]
Description=Zrn MBP Tickle Background Daemon
After=graphical.target

[Service]
Type=simple
ExecStart=/home/zerone/bin/zrn_perfd --daemon --allow-experimental-features
Restart=on-failure
RestartSec=5
Environment="DISPLAY=:0"

[Install]
WantedBy=multi-user.target
```

### Managing the Service

```bash
# Reload systemd configuration
sudo systemctl daemon-reload

# Start and enable on boot
sudo systemctl enable --now zrn-mbp-tickle.service

# Restart service after code changes
sudo systemctl restart zrn-mbp-tickle.service

# Check service status
systemctl status zrn-mbp-tickle.service

# Monitor live journal logs
journalctl -u zrn-mbp-tickle.service -f
```

---

## Configuration Files

| File | Description |
|---|---|
| `~/.zrnperformanceprofile` | Current power mode string (`None`, `Nominal`, `Moderate`, `High`). |
| `~/.zrnperformanceexempt` | Permanent exemption process names (one `comm` per line). |
| `~/.zrnperformancelimits` | Configured CPU limits (format: `comm:percent`). |
| `~/.zrn_switch_data.csv` | Dataset of recorded window transition events. |
| `~/.zrn_model_weights.bin` | Binary neural network weights and bias tensors. |
| `/tmp/boostml.csv` | Telemetry dataset for RapidBoost and CPU usage. |
| `/tmp/zrn_perf.lock` | Daemon single-instance lockfile. |

