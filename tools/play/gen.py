#!/usr/bin/env python3
"""Scripted play: a small script language -> the runtime's NX_INPUT string (runtime/src/hle_hid.cpp).

NX_INPUT holds "<frame>:<what>" items, each held for 8 frames from that frame: a button (A B X Y L R ZL ZR Plus
Minus Up Down Left Right), the left stick at full tilt (LUp LDown LLeft LRight; two on one frame make a diagonal),
a touch T<x>x<y> or a mouse move M<x>x<y> (1280x720 screen). The same syntax, with frames counted from when it is
read, goes into the data folder's input_live file of a running game (live.py).

Script lines (one command each; '#' starts a comment; frames are presented frames, 30 per second in game):
  at <frame>                      move the cursor to an absolute frame
  wait <frames>                   move the cursor forward
  press <button> [times] [gap]    tap a button (8 frames held), `times` times, `gap` frames apart (default 20)
  hold <button> <frames>          hold a button
  walk <dir> <frames> [run] [+Y]  tilt the stick: up down left right, or up-left, down-right...; `run` holds B too,
                                  +<button> holds another one (+Y: pick up what is walked over)
  touch <x> <y> [gap]             tap the touch screen (default gap 20)
  mouse <x> <y>                   move the mouse there (hover, button up)
  include <file>                  the lines of another script (path relative to this one, or to tools/play)
  mark <name>                     print the frame reached at this point (stderr)

usage: gen.py <script> [--base N]        prints the NX_INPUT string; stderr: "end <frame>" and the marks
"""
import os
import sys

BUTTONS = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Plus", "Minus", "Up", "Down", "Left", "Right",
           "LUp", "LDown", "LLeft", "LRight"}
DIRS = {"up": ["LUp"], "down": ["LDown"], "left": ["LLeft"], "right": ["LRight"],
        "up-left": ["LUp", "LLeft"], "up-right": ["LUp", "LRight"],
        "down-left": ["LDown", "LLeft"], "down-right": ["LDown", "LRight"]}
HOLD = 8  # frames each NX_INPUT item is held
HERE = os.path.dirname(os.path.abspath(__file__))


class Gen:
    def __init__(self):
        self.t = 0
        self.items = []
        self.marks = []

    def add(self, frame, what):
        self.items.append((frame, what))

    def held(self, what, frames):
        """`what` held for `frames` frames, as back-to-back items (the last one may run a few frames over)."""
        for f in range(self.t, self.t + max(frames, 1), HOLD):
            for w in what:
                self.add(f, w)

    def button(self, b):
        if b not in BUTTONS:
            raise ValueError(f"unknown button {b}")
        return b

    def run_file(self, path, depth=0):
        if depth > 16:
            raise ValueError("include nested too deep")
        with open(path) as f:
            for n, line in enumerate(f, 1):
                try:
                    self.line(line, os.path.dirname(path), depth)
                except (ValueError, IndexError) as e:
                    raise SystemExit(f"{path}:{n}: {e}: {line.strip()}")

    def run_text(self, text, where):
        for line in text.replace(";", "\n").splitlines():
            self.line(line, where, 0)

    def line(self, line, where, depth):
        w = line.split("#", 1)[0].split()
        if not w:
            return
        cmd, a = w[0].lower(), w[1:]
        if cmd == "at":
            self.t = int(a[0])
        elif cmd == "wait":
            self.t += int(a[0])
        elif cmd == "press":
            b = self.button(a[0])
            times = int(a[1]) if len(a) > 1 else 1
            gap = int(a[2]) if len(a) > 2 else 20
            for _ in range(times):
                self.add(self.t, b)
                self.t += max(gap, HOLD + 1)
        elif cmd == "hold":
            self.held([self.button(a[0])], int(a[1]))
            self.t += int(a[1])
        elif cmd == "walk":
            d = DIRS[a[0].lower()]
            frames = int(a[1])
            extra = [self.button(x[1:]) for x in a[2:] if x.startswith("+")]
            self.held(d + (["B"] if "run" in a[2:] else []) + extra, frames)
            self.t += frames
        elif cmd == "touch":
            self.add(self.t, f"T{int(a[0])}x{int(a[1])}")
            self.t += max(int(a[2]) if len(a) > 2 else 20, HOLD + 1)
        elif cmd == "mouse":
            self.add(self.t, f"M{int(a[0])}x{int(a[1])}")
        elif cmd == "include":
            p = os.path.join(where, a[0])
            if not os.path.exists(p):
                p = os.path.join(HERE, a[0])
            self.run_file(p, depth + 1)
        elif cmd == "mark":
            self.marks.append((a[0], self.t))
        else:
            raise ValueError(f"unknown command {cmd}")

    def nx_input(self, base=0):
        return ",".join(f"{f + base}:{w}" for f, w in sorted(self.items, key=lambda x: x[0]))


def main():
    args = sys.argv[1:]
    base = 0
    if "--base" in args:
        i = args.index("--base")
        base = int(args[i + 1])
        del args[i:i + 2]
    if len(args) != 1:
        sys.exit(__doc__)
    g = Gen()
    g.run_file(args[0])
    print(g.nx_input(base))
    for name, t in g.marks:
        print(f"mark {name} {t + base}", file=sys.stderr)
    print(f"end {g.t + base}", file=sys.stderr)


if __name__ == "__main__":
    main()
