# nosleep

Prevent Windows from sleeping using the `SetThreadExecutionState` API.

## Features

- Prevent system sleep indefinitely or for a specified duration
- Optional display sleep prevention
- Away mode support for compatible hardware
- System tray mode with taskbar icon (optional)
- Toast notifications for timeouts and errors
- Configurable refresh intervals

## Installation

### Install dependencies
```bash
pip install pystray Pillow
```

## Usage

### System tray mode
Run the application to start with taskbar icon:
- Right-click icon to set duration (30min, 1h, 2h, custom, or indefinite)
- Use "Stop" menu item to stop sleep prevention
- Ctrl+C is disabled in tray mode - use tray menu to stop
- Notifications appear when timer expires
- Icon changes color to indicate active/inactive state (gray=idle, green=active)

## Command-line mode

The program can be run from the command line with arguments:

| Flag | Short | Description |
|------|-------|-------------|
| `--duration` | `-d` | Minutes to prevent sleep (positive integer; ≤0 = indefinite) |
| `--interval` | `-i` | Seconds between refreshes (default: 20) |
| `--prevent-display` | `-p` | Also keep display awake |
| `--away-mode` | `-a` | Enable away mode (hardware‑dependent) |
| `--verbose` | `-v` | Print detailed status on each refresh |
| `--tray` | `-t` | Launch the system‑tray GUI (default if no arguments) |

**Entry points:**
- `python -m nosleep [ARGS]` (run from project root directory)
- `python app.py [ARGS]` (run from inside the `nosleep` folder)

**Examples:**

```bash
# Run for 30 minutes, refresh every 10 seconds, keep display awake
python -m nosleep --duration 30 --interval 10 --prevent-display

# Run indefinitely with away mode enabled, verbose logging
python -m nosleep --away-mode --verbose

# Start the tray GUI (same as running without arguments)
python -m nosleep --tray
```

**Default behavior:** If no arguments are given, the program starts in tray mode, preserving backward compatibility.

## API
```python
from nosleep import NoSleep

ns = NoSleep()
ns.run(duration_minutes=30)  # Run for 30 minutes
```

## Requirements
- Windows (uses Windows API)
- Python 3.6+
- Optional: pystray, Pillow (for tray mode)

