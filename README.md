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

