#!/usr/bin/env python3
"""
System tray integration for nosleep Windows utility
Provides taskbar icon with duration settings and notifications
"""

import threading
import time
import sys
import os
import signal
from datetime import datetime, timedelta
from typing import Optional, Callable

# Conditional imports for GUI components
try:
    import pystray
    from PIL import Image, ImageDraw
    GUI_AVAILABLE = True
except ImportError:
    GUI_AVAILABLE = False
    pystray = None
    Image = None
    ImageDraw = None

try:
    from .core import NoSleep
except ImportError:
    # For standalone testing
    from core import NoSleep


class NoSleepTray:
    """System tray manager for nosleep application"""

    def __init__(self):
        if not GUI_AVAILABLE:
            raise ImportError("pystray and Pillow are required for tray mode")

        self.nosleep: Optional[NoSleep] = None
        self.duration_minutes: Optional[int] = None
        self.start_time: Optional[datetime] = None
        self.is_running = False
        self.timer_thread: Optional[threading.Thread] = None
        self.stop_event = threading.Event()

        # Create system tray icon
        self.icon = pystray.Icon("nosleep")
        self.setup_icon()
        self.setup_menu()

    def create_image(self, color: str = "gray", remaining_seconds: Optional[int] = None, total_seconds: Optional[int] = None) -> Image.Image:
        """Create an icon image with specified color and optional number display for remaining minutes"""
        if remaining_seconds is not None and total_seconds is not None and total_seconds > 0:
            # Show number only - transparent background
            # Create RGBA image with transparent background
            image = Image.new('RGBA', (128, 128), (0, 0, 0, 0))
            draw = ImageDraw.Draw(image)
            
            # Calculate minutes (show 0 when less than 1 minute)
            minutes = max(0, remaining_seconds // 60)
            time_text = str(minutes)
            
            # Determine font size to fill the icon area
            # Start with large font and shrink if needed
            font_size = 80
            font = None
            try:
                from PIL import ImageFont
                # Try to find a suitable font
                try:
                    font = ImageFont.truetype("arial.ttf", font_size)
                except:
                    # Fallback to default font
                    font = ImageFont.load_default()
            except ImportError:
                pass
            
            # Adjust font size to fit within icon bounds
            bbox = draw.textbbox((0, 0), time_text, font=font) if font else draw.textbbox((0, 0), time_text)
            text_width = bbox[2] - bbox[0]
            text_height = bbox[3] - bbox[1]
            
            # Scale down if text is too wide or tall
            max_width = 110  # Leave some margin
            max_height = 110
            if text_width > max_width or text_height > max_height:
                scale = min(max_width / text_width, max_height / text_height)
                font_size = int(font_size * scale)
                if font:
                    try:
                        font = ImageFont.truetype("arial.ttf", font_size)
                    except:
                        font = ImageFont.load_default()
            
            # Recalculate bbox with adjusted font
            bbox = draw.textbbox((0, 0), time_text, font=font) if font else draw.textbbox((0, 0), time_text)
            text_width = bbox[2] - bbox[0]
            text_height = bbox[3] - bbox[1]
            
            # Center text
            text_x = (128 - text_width) // 2
            text_y = (128 - text_height) // 2
            
            # Draw text in white
            draw.text((text_x, text_y), time_text, fill="white", font=font)
            
            # Set icon title with detailed time
            if remaining_seconds >= 3600:
                hours = remaining_seconds // 3600
                minutes = (remaining_seconds % 3600) // 60
                self.icon.title = f"nosleep - {hours}h {minutes}m remaining"
            elif remaining_seconds >= 60:
                minutes = remaining_seconds // 60
                seconds = remaining_seconds % 60
                self.icon.title = f"nosleep - {minutes}m {seconds}s remaining"
            else:
                self.icon.title = f"nosleep - {remaining_seconds}s remaining"
        else:
            # Draw Z symbol with colored background (indefinite or inactive)
            # Create RGB image with solid color
            image = Image.new('RGB', (128, 128), color)
            draw = ImageDraw.Draw(image)
            
            # Draw a simple "Z" symbol for sleep prevention
            draw.line((40, 40, 88, 40), fill="white", width=6)  # Top
            draw.line((88, 40, 40, 88), fill="white", width=6)  # Diagonal
            draw.line((40, 88, 88, 88), fill="white", width=6)  # Bottom
            
            # Set default title
            if color == "green":
                self.icon.title = "nosleep - Active (indefinite)"
            else:
                self.icon.title = "nosleep - System sleep prevention"

        # Resize to typical tray icon size
        image = image.resize((32, 32), Image.Resampling.LANCZOS)
        return image

    def setup_icon(self):
        """Setup icon with default state"""
        self.icon.icon = self.create_image("gray")
        self.icon.title = "nosleep - System sleep prevention"

    def update_icon(self):
        """Update the tray icon with current time remaining"""
        if self.is_running and self.duration_minutes is not None and self.start_time is not None:
            total_seconds = self.duration_minutes * 60
            elapsed = (datetime.now() - self.start_time).total_seconds()
            remaining_seconds = max(0, total_seconds - elapsed)
            
            # Update icon with number display
            self.icon.icon = self.create_image(
                "green", 
                int(remaining_seconds), 
                total_seconds
            )
        elif self.is_running:
            # Running indefinitely (no duration)
            self.icon.icon = self.create_image("green")
        else:
            # Not running
            self.icon.icon = self.create_image("gray")

    def get_menu(self):
        """Get the current menu based on running state"""
        return pystray.Menu(
            pystray.MenuItem("Set Duration", self.create_duration_menu()),
            pystray.MenuItem("Stop", self.stop_nosleep, enabled=self.is_running),
            pystray.MenuItem("Exit", self.cleanup)
        )

    def setup_menu(self):
        """Setup right-click menu"""
        self.icon.menu = self.get_menu()

    def create_duration_menu(self):
        """Create submenu for duration settings"""
        return pystray.Menu(
            pystray.MenuItem("30 minutes", lambda: self.set_duration(30)),
            pystray.MenuItem("1 hour", lambda: self.set_duration(60)),
            pystray.MenuItem("2 hours", lambda: self.set_duration(120)),
            pystray.MenuItem("Custom...", self.show_custom_dialog),
            pystray.MenuItem("Indefinite", lambda: self.set_duration(None))
        )

    def show_custom_dialog(self):
        """Show custom duration input dialog using console input (GUI dialogs have focus issues in tray apps)"""
        try:
            print("\n" + "="*50)
            print("nosleep - Custom Duration")
            print("="*50)
            print("Enter the number of minutes to prevent system sleep.")
            print("Valid range: 1 to 1440 minutes (24 hours)")
            print("Enter 'cancel' to cancel.")
            print("="*50)

            while True:
                try:
                    user_input = input("Duration in minutes: ").strip()

                    if user_input.lower() in ('cancel', 'quit', 'exit'):
                        print("Cancelled.")
                        self.show_notification("Cancelled", "Custom duration input cancelled")
                        return

                    minutes = int(user_input)

                    if 1 <= minutes <= 1440:
                        self.set_duration(minutes)
                        print(f"Set to {minutes} minutes.")
                        return
                    else:
                        print(f"Error: {minutes} is not in range 1-1440. Please try again.")
                except ValueError:
                    print("Error: Please enter a valid number (1-1440).")
        except (EOFError, KeyboardInterrupt):
            print("\nInput cancelled.")
            self.show_notification("Input Cancelled", "Custom duration input was cancelled")

    def set_duration(self, minutes: Optional[int]):
        """Set duration and start nosleep"""
        self.duration_minutes = minutes

        if minutes is None:
            duration_text = "indefinitely"
        else:
            duration_text = f"for {minutes} minute{'s' if minutes != 1 else ''}"

        self.show_notification("Starting", f"Preventing system sleep {duration_text}")
        self.start_nosleep()

    def start_nosleep(self):
        """Start the nosleep prevention with current duration"""
        if self.is_running:
            self.stop_nosleep()

        # Start nosleep instance
        self.nosleep = NoSleep()
        self.start_time = datetime.now()
        self.is_running = True
        self.stop_event.clear()

        # Update UI state
        self.update_icon()
        self.icon.menu = self.get_menu()

        # Start timer thread if duration is set
        if self.duration_minutes is not None:
            # Ensure any previous timer thread is cleaned up
            if self.timer_thread and self.timer_thread.is_alive():
                if threading.current_thread() != self.timer_thread:
                    self.timer_thread.join(timeout=0.5)
                self.timer_thread = None

            self.timer_thread = threading.Thread(
                target=self._duration_timer,
                args=(self.duration_minutes * 60,),  # Convert to seconds
                daemon=True
            )
            self.timer_thread.start()

        # Start nosleep in background thread
        # Ensure any previous nosleep thread is cleaned up
        if hasattr(self, 'nosleep_thread') and self.nosleep_thread and self.nosleep_thread.is_alive():
            if threading.current_thread() != self.nosleep_thread:
                self.nosleep_thread.join(timeout=0.5)
            self.nosleep_thread = None

        self.nosleep_thread = threading.Thread(
            target=self._run_nosleep,
            daemon=True
        )
        self.nosleep_thread.start()

    def _run_nosleep(self):
        """Run nosleep in background thread"""
        try:
            self.nosleep.run(
                duration_minutes=self.duration_minutes,
                interval_seconds=20,
                prevent_display=False,
                away_mode=False,
                verbose=False
            )
        except Exception as e:
            self.show_notification("Error", f"Failed to prevent sleep: {str(e)}")
            self.stop_nosleep()
        finally:
            # If nosleep.run() completed normally (duration reached or stopped),
            # ensure the tray state is updated
            if self.is_running:
                self.stop_nosleep()

    def _duration_timer(self, duration_seconds: int):
        """Timer thread that waits for duration and stops nosleep based on clock time"""
        # Use the start_time stored in instance to calculate elapsed time
        while not self.stop_event.is_set():
            if self.start_time is None:
                break
                
            elapsed = (datetime.now() - self.start_time).total_seconds()
            if elapsed >= duration_seconds:
                # Duration reached, stop nosleep
                self.on_timeout()
                break
            
            # Update icon with remaining time
            self.update_icon()
            
            # Wait for short interval or stop event
            remaining = max(0, duration_seconds - elapsed)
            # Wait up to 1 second at a time to be responsive to stop event
            wait_time = min(1.0, remaining)
            if wait_time > 0:
                self.stop_event.wait(wait_time)

    def on_timeout(self):
        """Called when duration timer expires"""
        elapsed = datetime.now() - self.start_time
        hours, remainder = divmod(int(elapsed.total_seconds()), 3600)
        minutes, seconds = divmod(remainder, 60)

        if hours > 0:
            elapsed_text = f"{hours}h {minutes}m"
        else:
            elapsed_text = f"{minutes}m {seconds}s"

        self.show_notification(
            "Time's up!",
            f"Sleep prevention stopped\nDuration: {elapsed_text}"
        )
        self.stop_nosleep()

    def stop_nosleep(self):
        """Stop nosleep prevention"""
        if not self.is_running:
            return

        self.is_running = False
        self.stop_event.set()

        try:
            # Wait for timer thread to finish
            if self.timer_thread and self.timer_thread.is_alive():
                if threading.current_thread() != self.timer_thread:
                    try:
                        self.timer_thread.join(timeout=2.0)
                    except Exception as e:
                        print(f"Warning: Failed to join timer thread: {e}")
                # Only set to None if thread is no longer alive
                if not self.timer_thread.is_alive():
                    self.timer_thread = None

            # Wait for nosleep thread to finish
            if hasattr(self, 'nosleep_thread') and self.nosleep_thread and self.nosleep_thread.is_alive():
                if threading.current_thread() != self.nosleep_thread:
                    try:
                        self.nosleep_thread.join(timeout=2.0)
                    except Exception as e:
                        print(f"Warning: Failed to join nosleep thread: {e}")
                # Only set to None if thread is no longer alive
                if not self.nosleep_thread.is_alive():
                    self.nosleep_thread = None

            # Stop nosleep instance
            if self.nosleep:
                self.nosleep.stop()
                self.nosleep = None
        finally:
            # Update UI state
            self.update_icon()
            self.icon.menu = self.get_menu()

        # Show notification if manually stopped
        if self.start_time:
            elapsed = datetime.now() - self.start_time
            hours, remainder = divmod(int(elapsed.total_seconds()), 3600)
            minutes, seconds = divmod(remainder, 60)

            if hours > 0:
                elapsed_text = f"{hours}h {minutes}m"
            else:
                elapsed_text = f"{minutes}m {seconds}s"

            self.show_notification(
                "Stopped",
                f"Sleep prevention manually stopped\nTotal duration: {elapsed_text}"
            )


    def show_notification(self, title: str, message: str):
        """Show Windows toast notification"""
        try:
            self.icon.notify(
                title=f"nosleep - {title}",
                message=message
            )
        except Exception as e:
            # Fallback to console if notification fails
            print(f"[Notification] {title}: {message}")

    def cleanup(self):
        """Cleanup resources and exit"""
        self.stop_nosleep()
        self.icon.stop()

    def run(self):
        """Start the system tray application"""
        self.icon.run()


def run_tray():
    """Entry point for tray mode"""
    if not GUI_AVAILABLE:
        print("Error: Tray mode requires pystray and Pillow packages")
        print("Install them with: pip install pystray Pillow")
        return 1

    # Ignore Ctrl+C in tray mode - stopping is only allowed via tray menu
    signal.signal(signal.SIGINT, signal.SIG_IGN)

    try:
        tray = NoSleepTray()
        tray.run()
        return 0
    except Exception as e:
        print(f"Failed to start tray application: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(run_tray())