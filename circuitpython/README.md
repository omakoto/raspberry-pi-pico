# CircuitPython Development

This directory contains CircuitPython projects, scripts, and helper utilities for Raspberry Pi Pico development.

*Note: Unless specified otherwise, the default target hardware for scripts in this directory is the Raspberry Pi Pico 2 W.*

## How monitor stdout

```bash
picocom -b 115200 /dev/ttyACM0
# ctrl-a q to quit
```

## Code Conventions

- **Shebang Line**: Every main CircuitPython script should start with the shebang:
  ```python
  #!/usr/bin/env circuit-run
  ```
- **Executable Permission**: Always mark the script as executable so that it can run directly:
  ```bash
  chmod +x <script-name>.py
  ```
