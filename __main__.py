#!/usr/bin/env python3
"""
Entry point for nosleep module
"""

import sys
import argparse
import logging
from .core import NoSleep
from .tray import run_tray


def run_cli():
    """Run nosleep from command line with arguments"""
    parser = argparse.ArgumentParser(
        description="Prevent Windows from sleeping using SetThreadExecutionState API",
        epilog="Example: python -m nosleep --duration 30 --prevent-display"
    )
    
    # Duration argument
    parser.add_argument(
        "--duration", "-d",
        type=int,
        help="Duration in minutes to prevent sleep (positive integer, 0 or negative = indefinite)"
    )
    
    # Interval argument
    parser.add_argument(
        "--interval", "-i",
        type=int,
        default=20,
        help="Interval in seconds to refresh sleep prevention (default: 20)"
    )
    
    # Flags
    parser.add_argument(
        "--prevent-display", "-p",
        action="store_true",
        help="Also prevent display from sleeping"
    )
    
    parser.add_argument(
        "--away-mode", "-a",
        action="store_true",
        help="Enable away mode (requires compatible hardware)"
    )
    
    parser.add_argument(
        "--verbose", "-v",
        action="store_true",
        help="Print detailed status on every refresh"
    )
    
    # Tray mode
    parser.add_argument(
        "--tray", "-t",
        action="store_true",
        help="Start in system tray mode (default if no arguments provided)"
    )
    
    args = parser.parse_args()
    
    # Convert non-positive duration to None (indefinite)
    if args.duration is not None and args.duration <= 0:
        args.duration = None
    
    # If tray flag is set or no arguments provided, run tray mode
    if args.tray or len(sys.argv) == 1:
        return run_tray()
    
    # Otherwise run CLI mode with provided arguments
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s - %(levelname)s - %(message)s"
    )
    
    ns = NoSleep()
    try:
        ns.run(
            duration_minutes=args.duration,
            interval_seconds=args.interval,
            prevent_display=args.prevent_display,
            away_mode=args.away_mode,
            verbose=args.verbose
        )
        return 0
    except KeyboardInterrupt:
        print("\nInterrupted by user")
        return 0
    except Exception as e:
        logging.error(f"Error: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(run_cli())