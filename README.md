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

### Core functionality (command-line only)
```bash
pip install .
```

### With system tray support
```bash
pip install pystray Pillow
```

## Usage

### Command-line mode
```bash
# Run indefinitely until Ctrl+C (CLI mode)
python -m nosleep --forever

# Run for 30 minutes (CLI mode)
python -m nosleep -d 30

# Run with system tray icon (default when no arguments)
python -m nosleep --tray
```

### System tray mode
Run with `--tray` flag to start with taskbar icon, or run without arguments to default to tray mode:
- Right-click icon to set duration (30min, 1h, 2h, custom, or indefinite)
- Use "Stop" menu item to stop sleep prevention
- Ctrl+C is disabled in tray mode - use tray menu to stop
- Notifications appear when timer expires
- Icon changes color to indicate active/inactive state (gray=idle, green=active)

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

## Version History

### 1.2.0 (2026-02-15)
- Added system tray mode with taskbar icon
- Added toast notifications for timeouts and errors
- Added duration settings via right-click menu
- Added custom duration input dialog
- Updated CLI with `--tray` flag
- Default to tray mode when no arguments provided

### 1.1.0
- Initial release with core functionality