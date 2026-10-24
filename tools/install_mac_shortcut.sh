#!/bin/bash
# Puts "Jogar ACNH.command" on the desktop: a double-click rebuilds what changed and starts the game.
cd "$(dirname "$0")/.." || exit 1
pwd > "$HOME/.acnhrecomp_repo"
cp "Jogar ACNH.command" "$HOME/Desktop/Jogar ACNH.command"
chmod +x "$HOME/Desktop/Jogar ACNH.command" "Jogar ACNH.command"
xattr -d com.apple.quarantine "$HOME/Desktop/Jogar ACNH.command" 2>/dev/null
echo "Shortcut on the desktop: Jogar ACNH.command"
