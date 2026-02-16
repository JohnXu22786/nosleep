#!/usr/bin/env python3
"""
nosleep - A command-line tool to prevent Windows from sleeping
Uses Windows SetThreadExecutionState API to keep system awake
"""

import ctypes
import sys
import time
import argparse
import signal
import threading
import logging
from datetime import datetime, timedelta

# Windows API constants
ES_CONTINUOUS = 0x80000000
ES_SYSTEM_REQUIRED = 0x00000001
ES_DISPLAY_REQUIRED = 0x00000002
ES_AWAYMODE_REQUIRED = 0x00000040


class NoSleep:
    """Main class for preventing system sleep"""

    def __init__(self):
        self.running = False
        self.start_time = None
        self.thread = None

    def prevent_sleep(self, prevent_display=False, away_mode=False, silent=False):
        """Call Windows API to prevent sleep with optional flags

        Args:
            prevent_display: Also prevent display from sleeping
            away_mode: Enable away mode (requires specific hardware/power settings)
            silent: If True, don't print informational messages (only errors)
        """
        try:
            flags = ES_CONTINUOUS | ES_SYSTEM_REQUIRED

            if prevent_display:
                flags |= ES_DISPLAY_REQUIRED
                if not silent:
                    logging.info("Also preventing display sleep")

            if away_mode:
                flags |= ES_AWAYMODE_REQUIRED
                if not silent:
                    logging.info("Away mode enabled (may require specific hardware)")

            # Try up to 2 times with a short delay between attempts
            for attempt in range(2):
                try:
                    # Set thread execution state to prevent sleep
                    result = ctypes.windll.kernel32.SetThreadExecutionState(flags)
                    if result != 0:
                        return True

                    # If result is 0 but no exception, API call failed
                    if attempt == 0 and not silent:
                        logging.warning("Sleep prevention failed, retrying...")
                except Exception as e:
                    if attempt == 0 and not silent:
                        logging.warning(f"Sleep prevention error: {e}, retrying...")

                # Wait before retry (only if first attempt failed)
                if attempt == 0:
                    time.sleep(0.5)

            # Both attempts failed
            logging.error("Sleep prevention failed after retry")
            return False
        except Exception as e:
            logging.error(f"Error preventing sleep: {e}")
            return False

    def allow_sleep(self):
        """Restore normal sleep behavior"""
        try:
            result = ctypes.windll.kernel32.SetThreadExecutionState(ES_CONTINUOUS)
            return result != 0
        except Exception as e:
            logging.error(f"Error allowing sleep: {e}")
            return False

    def run(self, duration_minutes=None, interval_seconds=20, prevent_display=False, away_mode=False, verbose=False):
        """Run nosleep for specified duration or indefinitely with regular refresh

        Args:
            duration_minutes: Number of minutes to run (None = indefinitely)
            interval_seconds: Interval in seconds to refresh sleep prevention state
            prevent_display: Also prevent display from sleeping
            away_mode: Enable away mode
            verbose: If True, print status on every refresh
        """
        self.running = True
        self.start_time = datetime.now()

        logging.info("nosleep started - preventing system sleep")
        print("Press Ctrl+C to stop and allow sleep")

        if duration_minutes:
            end_time = self.start_time + timedelta(minutes=duration_minutes)
            logging.info(f"Will run for {duration_minutes} minutes (until {end_time.strftime('%H:%M:%S')})")

        # Prevent sleep initially
        if not self.prevent_sleep(prevent_display, away_mode):
            logging.warning("Failed to prevent sleep. Try running as administrator.")

        # Main loop
        try:
            # Display refresh interval info
            logging.info(f"Refreshing sleep prevention every {interval_seconds} seconds")


            refresh_count = 0
            failure_count = 0
            MAX_FAILURES = 5  # Maximum consecutive failures before giving up

            while self.running:
                # Refresh sleep prevention state
                refresh_count += 1
                success = self.prevent_sleep(prevent_display, away_mode, silent=True)

                if success:
                    failure_count = 0  # Reset failure count on success
                    # Print status (every refresh if verbose, otherwise every 2 refreshes)
                    if verbose or refresh_count % 2 == 0:
                        elapsed = datetime.now() - self.start_time
                        minutes = int(elapsed.total_seconds() // 60)
                        seconds = int(elapsed.total_seconds() % 60)
                        if verbose:
                            logging.info(f"[{datetime.now().strftime('%H:%M:%S')}] Active: {minutes}m {seconds}s (#{refresh_count})")
                        else:
                            logging.info(f"Status: Active for {minutes}m {seconds}s (refresh #{refresh_count})")
                else:
                    failure_count += 1
                    logging.warning(f"Failed to refresh sleep prevention (attempt #{refresh_count}, failure #{failure_count})")

                    # If too many consecutive failures, exit
                    if failure_count >= MAX_FAILURES:
                        logging.error(f"{MAX_FAILURES} consecutive failures. Exiting...")
                        self.running = False
                        break

                # Sleep for the specified interval
                time.sleep(interval_seconds)

                # Check if duration has elapsed
                if duration_minutes and datetime.now() >= end_time:
                    logging.info(f"Duration reached ({duration_minutes} minutes). Stopping...")
                    break

        except KeyboardInterrupt:
            print("\nInterrupted by user")
        finally:
            self.stop()

    def stop(self):
        """Stop nosleep and allow sleep"""
        if self.running:
            self.running = False
            if self.allow_sleep():
                logging.info("Sleep behavior restored")
            else:
                logging.warning("Failed to restore sleep behavior")

            runtime = datetime.now() - self.start_time if self.start_time else timedelta(0)
            logging.info(f"nosleep ran for {runtime}")

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
    import platform
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