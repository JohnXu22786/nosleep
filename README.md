# nosleep

A lightweight Windows utility written in C that prevents system sleep using the `SetThreadExecutionState` API. It can run indefinitely, for a specified duration, or as a system tray application with configurable options.

## Features

* **Flexible Sleep Prevention**: Prevent system sleep indefinitely or for a specified duration.
* **Display Sleep Prevention**: Optional display sleep prevention.
* **Away Mode Support**: Away mode support for compatible hardware.
* **System Tray Mode**: System tray mode with taskbar icon (optional).
* **Notifications**: Toast notifications for timeouts and errors.
* **Configurable Refresh Intervals**: Configurable refresh intervals (default: 20 seconds).
* **Verbose Logging**: Print detailed status on each refresh.

## System Requirements

* **Operating System**: Windows (uses Windows API functions).
* **Compiler**: MinGW gcc compiler suite (including `windres` resource compiler).
* **Build Tools**: Basic command-line tools (`make` or `mingw32-make` on Windows).

## Installation & Compilation

### Using the Provided Makefile

The project includes a `Makefile` for easy building. Open a terminal (e.g., MinGW shell) in the project root directory and run:

```bash
make          # Compile the executable (output: bin/nosleep.exe)
make clean    # Remove build artifacts (obj/ and bin/ directories)
make run      # Compile and run the program in tray mode
make test     # Compile and run the program with --help to verify
```

**Windows Note**: If the `make` command is not available, try using `mingw32-make` instead:

```bash
mingw32-make          # Compile the executable
mingw32-make clean    # Remove build artifacts
mingw32-make run      # Compile and run in tray mode
```

### Manual Compilation

If you prefer to compile manually, you can use the following commands (ensure MinGW's `gcc` and `windres` are in your PATH):

```bash
gcc -std=c99 -Wall -Wextra -O2 -Isrc -c src/core.c -o obj/core.o
gcc -std=c99 -Wall -Wextra -O2 -Isrc -c src/tray.c -o obj/tray.o
gcc -std=c99 -Wall -Wextra -O2 -Isrc -c src/main.c -o obj/main.o
windres src/resources.rc -o obj/resources.o
gcc obj/core.o obj/tray.o obj/main.o obj/resources.o -o bin/nosleep.exe -mwindows -luser32 -lkernel32 -lgdi32 -lpowrprof -ladvapi32
```

### Adding to PATH (Optional)

To run `nosleep.exe` from any directory without specifying the full path:

1. Copy `bin/nosleep.exe` to a directory already in your PATH (e.g., `C:\Windows\System32`), or
2. Add the `bin` directory to your system PATH:

   **Via Command Prompt (Administrator)**:
   ```cmd
   setx PATH "%PATH%;D:\Administrator\Desktop\Agent\nosleep\bin"
   ```
   
   Replace the path with the actual location of your `bin` directory.

   **Via System Properties**:
   - Open System Properties → Advanced → Environment Variables
   - Under "System variables", edit "Path"
   - Add the full path to the `bin` directory

After adding to PATH, you can run `nosleep.exe` from any command prompt.

## Usage

### Entry Points
- `bin/nosleep.exe [ARGS]` - Run the compiled executable from the project root directory
- `nosleep.exe [ARGS]` - Run from the current directory if the executable is in PATH or copied to current directory

### Command-Line Mode

Run `nosleep.exe` with the following arguments:

| Flag | Short | Description |
|------|-------|-------------|
| `--duration MINUTES` | `-d` | Minutes to prevent sleep (positive integer; 0 or negative = indefinite) |
| `--interval SECONDS` | `-i` | Seconds between refreshes (default: 20) |
| `--prevent-display` | `-p` | Also keep the display awake |
| `--away-mode` | `-a` | Enable away mode (hardware‑dependent) |
| `--verbose` | `-v` | Print detailed status on each refresh |
| `--tray` | `-t` | Launch the system‑tray GUI |
| `--help` | `-h` | Show this help message |

**Default behavior**: If no arguments are given, the program starts in tray mode, preserving backward compatibility.

#### Examples

```bash
# Prevent sleep for 30 minutes, also keep display awake
nosleep.exe --duration 30 --prevent-display

# Prevent sleep for 60 minutes, refresh every 10 seconds, verbose logging
nosleep.exe -d 60 -i 10 -v

# Enable away mode and run indefinitely
nosleep.exe --away-mode

# Start the tray GUI (same as running without arguments)
nosleep.exe --tray
```

### System Tray Mode

When running in tray mode (default or with `--tray`), the application places an icon in the system tray with the following features:

* **Right-click the icon** to access the context menu
* **Set duration** from the menu:
  - 30 minutes
  - 1 hour
  - 2 hours
  - Custom... (enter custom duration in minutes)
  - Indefinite (run until manually stopped)
* **Use "Stop" menu item** to stop sleep prevention
* **Ctrl+C is disabled in tray mode** - use tray menu to stop
* **Icon color indicates status**:
  - Gray = idle/inactive
  - Green = active (preventing sleep)
* **Toast notifications appear** when timer expires or errors occur
* **"When finished" options** (configurable behavior after timer expires):
  - None: Just stop preventing sleep (default)
  - Shutdown: Shut down the computer
  - Sleep: Put the computer to sleep
* **Exit the application** via the tray menu

**Note**: The application runs as a Windows GUI application when in tray mode. Ctrl+C is disabled in this mode - use the tray menu's "Stop" or "Exit" options. To exit the program completely, use the tray menu's "Exit" option.

## Build Options (Makefile Targets)

The `Makefile` provides several convenient targets:

* `make` or `make all`: Build the executable (`bin/nosleep.exe`).
* `make clean`: Remove all object files and the executable.
* `make run`: Build and run the program in tray mode.
* `make run-cli`: Build and run the program with a 30‑minute duration (CLI mode).
* `make test`: Build and run the program with `--help` to verify functionality.
* `make install`: Copy the executable to the current directory.

## Debugging

To enable debug output (prints additional information to the console), set the environment variable `NOSLEEP_DEBUG` to `1` before running the program:

```cmd
set NOSLEEP_DEBUG=1
nosleep.exe --duration 10 --verbose
```

This is especially useful when combined with the `--verbose` flag for troubleshooting.

## Contributing

Contributions are welcome! If you find a bug or have a feature request, please open an issue. If you'd like to contribute code, feel free to submit a pull request. Ensure your changes compile without warnings and follow the existing code style.

## License

This project is licensed under the GNU General Public License v3.0. See the [LICENSE](LICENSE) file for the full license text.