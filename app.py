#!/usr/bin/env python3
"""
nosleep - Prevent Windows from sleeping using SetThreadExecutionState API
Main application entry point
"""

import sys
import os
import traceback
import argparse
import logging


def parse_args():
    """Parse command line arguments"""
    parser = argparse.ArgumentParser(
        description="Prevent Windows from sleeping using SetThreadExecutionState API",
        epilog="Example: python app.py --duration 30 --prevent-display"
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
    
    return parser.parse_args()


def run_cli(args):
    """Run nosleep from command line with arguments"""
    from nosleep.core import NoSleep
    
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


def main_wrapper():
    """Wrapper to handle import errors and display helpful messages"""
    try:
        # Calculate paths for imports
        script_dir = os.path.dirname(os.path.abspath(__file__))
        parent_dir = os.path.dirname(script_dir)

        # Add parent directory to sys.path to allow importing nosleep package
        # This makes the nosleep package importable as "import nosleep"
        if parent_dir not in sys.path:
            sys.path.insert(0, parent_dir)

        # Import the tray function from the nosleep package
        from nosleep.tray import run_tray
        
        # Parse command line arguments
        args = parse_args()
        
        # Convert non-positive duration to None (indefinite)
        if args.duration is not None and args.duration <= 0:
            args.duration = None
        
        # If tray flag is set or no arguments provided, run tray mode
        if args.tray or len(sys.argv) == 1:
            return run_tray()
        
        # Otherwise run CLI mode with provided arguments
        return run_cli(args)

    except ImportError as e:
        print(f"Import Error: {e}", file=sys.stderr)
        print("\nDebug information:", file=sys.stderr)
        print(f"  Script directory: {script_dir}", file=sys.stderr)
        print(f"  Parent directory: {parent_dir}", file=sys.stderr)
        print(f"  sys.path:", file=sys.stderr)
        for p in sys.path[:5]:  # Show first 5 paths
            print(f"    {p}", file=sys.stderr)
        print("\nFull traceback:", file=sys.stderr)
        traceback.print_exc()

        # Wait for user input so the window doesn't close immediately
        print("\nPress Enter to exit...")
        try:
            input()
        except:
            pass
        return 1
    except Exception as e:
        print(f"Unexpected error: {e}", file=sys.stderr)
        traceback.print_exc()

        # Wait for user input so the window doesn't close immediately
        print("\nPress Enter to exit...")
        try:
            input()
        except:
            pass
        return 1


if __name__ == "__main__":
    sys.exit(main_wrapper())