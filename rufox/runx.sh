#!/bin/sh
Xvfb :99 -screen 0 1024x900x24 >/dev/null 2>&1 &
sleep 2
DISPLAY=:99 "$@"
