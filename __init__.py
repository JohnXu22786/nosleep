#!/usr/bin/env python3
"""
nosleep - Prevent Windows from sleeping using SetThreadExecutionState API
"""

from .core import NoSleep

__all__ = ["NoSleep"]