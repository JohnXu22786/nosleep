#!/usr/bin/env python3
"""
nosleep - Prevent Windows from sleeping using SetThreadExecutionState API
"""

from .core import NoSleep

__version__ = "1.2.0"
__all__ = ["NoSleep"]