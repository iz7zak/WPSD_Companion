Changelog: WPSD Live Caller Display (v1.0 ➔ v2.0)
A major architectural update transforming the static WPSD caller monitor into an interactive, self-adjusting dashboard with hardware touch control, dynamic dark mode, and an NTP-synced UTC clock.

🖥️ Display & Visual Overhaul
Dark Gray Header Bar: Shifted the top title banner from the default WPSD orange to a clean dark gray (TFT_DARKGREY) background with centered white text (WPSD HOTSPOT LIVE CALLER).

Clean Text Erasure: Added precise, localized background clearing (fillRect) for the Name and metadata lines before redrawing, completely eliminating text ghosting and visual artifacts during rapid callsign transitions.

Refined Layout Spacing: Reworked vertical positioning with dedicated constants (BANNER_Y, NAME_Y, DIVIDER_Y, META1_Y–META5_Y), adding generous vertical padding between metadata and the bottom clock area.

🌗 Dark Mode & Touch Controls
Light / Dark Theme Engine: Introduced full theme state management (darkMode, bgColor, fgColor, labelColor) supporting seamless toggling between clean white and true black themes.

XPT2046 Touch Integration: Integrated support for the XPT2046 resistive touch controller (using VSPI pins 25, 39, 32, 33, 36). Tapping the upper banner region of the display instantly triggers a theme toggle with built-in button debouncing (400ms).

☀️ Automated Sunrise / Sunset Solar Switching
Astronomical Calculations: Added a built-in solar geometry algorithm to calculate precise UTC sunrise and sunset times based on configurable geographic coordinates (OBS_LATITUDE, OBS_LONGITUDE, defaulting to Rome, Italy).

Smart Event Transitions: Automatically switches between Light and Dark themes at dawn and dusk based on NTP time updates, while retaining the ability to manually override via touch.

⏱️ NTP UTC Clock & Time Sync
Live UTC Readout: Added continuous NTP time synchronization (0.pool.ntp.org) running in the background with an hourly re-sync loop.

Bottom Status Bar: Rendered a live, centered UTC date and time ticker at the bottom of the screen (YYYY/Mon/DD - HH:MM:SS - UTC).

⚡ Performance & Rendering Optimizations
Selective / Differential Updates: Replaced full-screen redraw loops with targeted rendering checks. The display now only updates specific UI elements (callsign, name, DMR ID, target, mode, duration) when data actually changes, cutting down flicker and optimizing frame rendering.

Optimized PNG Buffer Scaling: Replaced static memory allocation with safe realloc handling for flag downloads, preventing memory fragmentation during continuous polling.

🌐 Network & Reliability
Background WiFi Health Checks: Implemented ensureWifiConnected() to periodically monitor link state and attempt background recovery if the ESP32 drops connection.

Isolated Client Instances: Separated standard local HTTP polling clients (localClient) from secure HTTPS RadioID API queries (radioIdClient) to prevent connection collision timeouts and resource locking.
