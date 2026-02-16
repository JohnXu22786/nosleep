#!/usr/bin/env python3
"""
nosleep - Prevent Windows from sleeping using SetThreadExecutionState API
"""

from .core import NoSleep

__version__ = "1.1.0"
__all__ = ["NoSleep"]