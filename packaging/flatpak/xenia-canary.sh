#!/bin/sh
# Xenia Canary inside the sandbox.
#
# The build forces GDK_BACKEND=x11 itself; this wrapper exists so the config,
# games and save states land somewhere sensible when no folders have been
# chosen yet, and so the bundled ReShade shaders are found.
exec /app/bin/xenia_canary "$@"
