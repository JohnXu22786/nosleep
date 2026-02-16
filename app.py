#!/usr/bin/env python3
"""
nosleep - Prevent Windows from sleeping using SetThreadExecutionState API
Main application entry point
"""

import sys
import os
import traceback

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

        # Run the tray function
        return run_tray()

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