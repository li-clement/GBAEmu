#!/bin/bash
# Kill existing GBAEmu process
pkill -9 GBAEmu || true

# Run the game
./bin/GBAEmu "$@"
