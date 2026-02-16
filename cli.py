#!/usr/bin/env python3
"""
Command-line interface for nosleep
"""

import argparse
import signal
import sys
import logging
import platform

from .core import NoSleep


def signal_handler(signum, frame):
    """Handle Ctrl+C"""
    print("\nReceived interrupt signal")
    sys.exit(0)


def main():
    """Main entry point"""
    parser = argparse.ArgumentParser(
        description="nosleep - Prevent Windows from sleeping using SetThreadExecutionState API",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  nosleep.py                      # Run indefinitely until Ctrl+C (refresh every 20s)
  nosleep.py -d 30               # Run for 30 minutes (refresh every 20s)
  nosleep.py -i 15               # Run indefinitely, refresh every 15 seconds
  nosleep.py -d 60 -i 30         # Run for 1 hour, refresh every 30 seconds
  nosleep.py --display           # Also prevent display from sleeping
  nosleep.py --awaymode          # Enable away mode (requires specific hardware)
  nosleep.py -d 60 --display -i 15  # Run for 1 hour, prevent display sleep, refresh every 15s

Notes:
  - May require administrator privileges on some systems
  - Windows 11 may have compatibility issues with this API
  - Use Ctrl+C to stop and restore normal sleep behavior
  - Regular refresh interval ensures sleep prevention remains active
        """
    )

    parser.add_argument("-d", "--duration", type=int, metavar="MINUTES",
                       help="Run for specified number of minutes")
    parser.add_argument("-i", "--interval", type=int, default=20, metavar="SECONDS",
                       help="Check and refresh sleep prevention interval in seconds (default: 20)")
    parser.add_argument("--forever", action="store_true",
                       help="Run indefinitely (default)")
    parser.add_argument("--display", action="store_true",
                       help="Also prevent display from sleeping")
    parser.add_argument("--awaymode", action="store_true",
                       help="Enable away mode (requires specific hardware/power settings)")
    parser.add_argument("--status", action="store_true",
                       help="Check if system sleep is currently prevented (not implemented)")
    parser.add_argument("--stop", action="store_true",
                       help="Stop nosleep if it's running (not implemented)")
    parser.add_argument("--verbose", action="store_true",
                       help="Print status on every refresh (more frequent updates)")
    parser.add_argument("--version", action="version", version="nosleep 1.1.0")

    args = parser.parse_args()

    # Configure logging
    logging.basicConfig(
        level=logging.INFO,
        format='%(asctime)s - %(levelname)s - %(message)s',
        datefmt='%H:%M:%S'
    )

    # Set up signal handler for Ctrl+C
    signal.signal(signal.SIGINT, signal_handler)

    # Check if running on Windows
    if not sys.platform.startswith('win'):
        logging.error("This tool only works on Windows")
        return 1

    # Handle status check
    if args.status:
        logging.info("Status check feature not yet implemented")
        logging.info("In future versions, this will check current execution state")
        return 0

    # Handle stop command
    if args.stop:
        logging.info("Stop command feature not yet implemented")
        logging.info("In future versions, this will signal a running instance")
        return 0

    # Determine duration
    duration = args.duration
    if args.forever or (not args.duration and not args.status and not args.stop):
        duration = None  # Run indefinitely

    # Show warning about Windows 11 compatibility
    if platform.release() == '11' or platform.version().startswith('10.0.22'):
        logging.warning("Windows 11 may have compatibility issues with SetThreadExecutionState API")
        logging.warning("If sleep prevention doesn't work, try running as administrator")

    # Validate interval
    if args.interval <= 0:
        logging.error(f"Interval must be positive, got {args.interval}")
        return 1

    # Run nosleep
    nosleep = NoSleep()
    nosleep.run(duration, args.interval, args.display, args.awaymode, args.verbose)

    return 0


if __name__ == "__main__":
    sys.exit(main())