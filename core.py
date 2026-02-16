#!/usr/bin/env python3
"""
Core NoSleep class for preventing Windows sleep using SetThreadExecutionState API
"""

import ctypes
import time
import logging
from datetime import datetime, timedelta

from . import constants


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
            flags = constants.ES_CONTINUOUS | constants.ES_SYSTEM_REQUIRED

            if prevent_display:
                flags |= constants.ES_DISPLAY_REQUIRED
                if not silent:
                    logging.info("Also preventing display sleep")

            if away_mode:
                flags |= constants.ES_AWAYMODE_REQUIRED
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
            result = ctypes.windll.kernel32.SetThreadExecutionState(constants.ES_CONTINUOUS)
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