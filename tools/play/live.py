#!/usr/bin/env python3
"""Drive a running game step by step: send input, wait for it to play out, look at a frame.

The game is started by run.sh (NX_LIVE_FRAME=1, its own data root). The runtime reads, in the data root:
  input_live   NX_INPUT items with frames counted from when it is read (checked every 15 frames, then deleted)
  dump_frames  "<count> <every>": dump the next presented frames to frames/NNNNN.ppm (checked every 60 frames)
and writes live_frame (the frame reached, every 15 frames).

usage: live.py <root> <command> [args]
  frame                         print the frame reached
  send "<script>" [--no-wait]   gen.py lines (';' separates them) or @file; waits until they have played out
  shot [name] [--full] [--crop x,y,w,h] [--minimap]
                                dump the next frame, convert it to a PNG (960 wide unless --full or a crop of the
                                1920x1080 frame) in $PLAY_SHOTS (default the folder given to run.sh, else
                                <root>/shots); prints its path
  go "<script>" [shot args]     send, then shot
  sweep "<script>" [n] [name]   send, dumping n frames (default 9) spread over it: one contact sheet, 3 a row
  wait <frames>                 wait until the game is <frames> further
  play <session> [last frame]   replay a recorded session (the root's sent.txt, or a scenario in that form) at
                                the frames it was recorded at
  alive                         exit 0 when the game process of this root is running
<root> is a name under private/play/roots or a path.
"""
import glob
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen  # noqa: E402

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def root_of(name):
    return name if os.path.isdir(name) and "/" in name else os.path.join(REPO, "private", "play", "roots", name)


def alive(root):
    pidf = os.path.join(root, "game.pid")
    try:
        pid = int(open(pidf).read())
        os.kill(pid, 0)
        return True
    except (OSError, ValueError):
        return False


def frame(root):
    try:
        return int(open(os.path.join(root, "live_frame")).read())
    except (OSError, ValueError):
        return -1


def need_alive(root):
    if not alive(root):
        sys.exit("live: the game is not running (see " + os.path.join(root, "nx.log") + ")")


def wait_frames(root, n, timeout=None):
    start = frame(root)
    goal = start + n
    t0 = time.time()
    limit = timeout if timeout is not None else 30 + n / 5  # 5 fps at worst
    while frame(root) < goal:
        need_alive(root)
        if time.time() - t0 > limit:
            sys.exit(f"live: timed out waiting for frame {goal} (at {frame(root)})")
        time.sleep(0.25)
    return frame(root)


def write_atomic(path, text):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write(text)
    os.rename(tmp, path)


def send(root, script, wait=True):
    need_alive(root)
    g = gen.Gen()
    if script.startswith("@"):
        g.run_file(script[1:])
    else:
        g.run_text(script, os.path.dirname(os.path.abspath(__file__)))
    path = os.path.join(root, "input_live")
    t0 = time.time()
    while os.path.exists(path):  # the previous one is not taken yet
        need_alive(root)
        if time.time() - t0 > 30:
            sys.exit("live: input_live is not being read")
        time.sleep(0.1)
    before = frame(root)
    write_atomic(path, g.nx_input() + "\n" if g.items else "0:None\n")
    with open(os.path.join(root, "sent.txt"), "a") as f:  # the session, to turn into a scenario later
        f.write(f"# frame {before}\n{script.replace(';', chr(10))}\n")
    while os.path.exists(path):
        need_alive(root)
        time.sleep(0.1)
    if wait:
        # The file is read at most 15 frames after `before`; the script's last item is held 8 frames.
        end = max(g.t, max((f for f, _ in g.items), default=0) + gen.HOLD)
        goal = before + 15 + end
        t0 = time.time()
        while frame(root) < goal:
            need_alive(root)
            if time.time() - t0 > 60 + end / 5:
                sys.exit("live: timed out waiting for the input to play out")
            time.sleep(0.2)
    return frame(root)


def play(root, path, until=None):
    """Replay a session recorded in sent.txt (or a scenario in that form): each block, headed "# frame N", is sent
    when the game is N - (first N) frames past the start of the replay. Blocks are not waited for, so the game's
    loads, which follow the clock, fall between them as when the session was recorded."""
    blocks = []
    for line in open(path):
        if line.startswith("# frame "):
            blocks.append([int(line.split()[2]), []])
        elif line.split("#", 1)[0].strip() and blocks:
            blocks[-1][1].append(line.strip())
    if not blocks:
        sys.exit(f"live: no '# frame N' blocks in {path}")
    while frame(root) < 0:  # the game has not read the pad yet
        need_alive(root)
        time.sleep(0.5)
    first, start = blocks[0][0], frame(root)
    if start < first:  # a game started for this replay: the recorded frames count from its start too
        first = start = 0
    for at, lines in blocks:
        if until is not None and at > until:
            break
        goal = start + at - first
        while frame(root) < goal:
            need_alive(root)
            time.sleep(0.2)
        send(root, ";".join(lines), wait=False)
        print(f"play: frame {frame(root)}: {'; '.join(lines)}", flush=True)
    return frame(root)


def shots_dir(root):
    d = os.environ.get("PLAY_SHOTS")
    if not d:
        try:
            d = open(os.path.join(root, "shots_dir")).read().strip()
        except OSError:
            d = os.path.join(root, "shots")
    os.makedirs(d, exist_ok=True)
    return d


def ppm_complete(p):
    try:
        with open(p, "rb") as f:
            head = f.read(64)
        parts = head.split(b"\n", 3)
        w, h = map(int, parts[1].split())
        return os.path.getsize(p) >= len(parts[0]) + len(parts[1]) + len(parts[2]) + 3 + w * h * 3
    except (OSError, ValueError, IndexError):
        return False


def crop_ppm(src, dst, box):
    """Cut box (x, y, w, h, in the frame's pixels) out of a binary PPM."""
    with open(src, "rb") as f:
        data = f.read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    px = parts[3]
    x, y, cw, ch = box
    x, y = max(0, min(x, w - 1)), max(0, min(y, h - 1))
    cw, ch = min(cw, w - x), min(ch, h - y)
    rows = [px[(y + r) * w * 3 + x * 3:(y + r) * w * 3 + (x + cw) * 3] for r in range(ch)]
    with open(dst, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (cw, ch) + b"".join(rows))


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]


def sheet(paths, out, scale=4, cols=3):
    """The frames at 1/scale (every scale-th pixel), `cols` to a row, into one PNG: a sweep at a glance."""
    tiles = []
    for p in paths:
        w, h, px = read_ppm(p)
        tw, th = w // scale, h // scale
        rows = []
        for y in range(th):
            r = px[y * scale * w * 3:(y * scale * w + w) * 3]
            o = bytearray(tw * 3)
            for k in range(3):
                o[k::3] = r[k::scale * 3][:tw]
            rows.append(bytes(o))
        tiles.append((tw, th, rows))
    tw, th = tiles[0][0], tiles[0][1]
    n_rows = (len(tiles) + cols - 1) // cols
    blank = bytes(tw * 3)
    lines = []
    for gr in range(n_rows):
        for y in range(th):
            line = b""
            for gc in range(cols):
                i = gr * cols + gc
                line += tiles[i][2][y] if i < len(tiles) and tiles[i][0] == tw else blank
            lines.append(line)
    tmp = out + ".ppm"
    with open(tmp, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (tw * cols, th * n_rows) + b"".join(lines))
    subprocess.run(["sips", "-s", "format", "png", tmp, "--out", out], check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    os.remove(tmp)
    return out


def sweep(root, script, count, name=None):
    """Send `script` and dump `count` frames spread over it; one contact sheet of them (left to right, top down)."""
    g = gen.Gen()
    g.run_text(script, os.path.dirname(os.path.abspath(__file__)))
    frames = os.path.join(root, "frames")
    os.makedirs(frames, exist_ok=True)
    old = set(glob.glob(os.path.join(frames, "*.ppm")))
    every = max(2, (g.t + 15) // count)
    write_atomic(os.path.join(root, "dump_frames"), f"{count} {every}\n")
    send(root, script)
    t0 = time.time()
    while True:
        new = sorted(p for p in glob.glob(os.path.join(frames, "*.ppm")) if p not in old and ppm_complete(p))
        if len(new) >= count or time.time() - t0 > 20:
            break
        time.sleep(0.5)
    if not new:
        sys.exit("live: no frames dumped")
    num = os.path.basename(new[0]).split(".")[0]
    out = sheet(new, os.path.join(shots_dir(root), f"{name or 'sweep'}_{num}.png"))
    for q in new:
        os.remove(q)
    return out


def shot(root, name=None, full=False, crop=None):
    need_alive(root)
    frames = os.path.join(root, "frames")
    os.makedirs(frames, exist_ok=True)
    old = set(glob.glob(os.path.join(frames, "*.ppm")))
    write_atomic(os.path.join(root, "dump_frames"), "1 1\n")
    t0 = time.time()
    new = []
    while True:
        need_alive(root)
        new = [p for p in glob.glob(os.path.join(frames, "*.ppm")) if p not in old and ppm_complete(p)]
        if new:
            break
        if time.time() - t0 > 60:
            sys.exit("live: no frame dumped within 60 s")
        time.sleep(0.2)
    time.sleep(0.2)
    p = sorted(new)[-1]
    num = os.path.basename(p).split(".")[0]
    out = os.path.join(shots_dir(root), f"{name or 'shot'}_{num}.png")
    src = p
    if crop:
        src = p + ".crop.ppm"
        crop_ppm(p, src, crop)
    cmd = ["sips", "-s", "format", "png"] + ([] if full or crop else ["-Z", "960"]) + [src, "--out", out]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for q in new + ([src] if crop else []):
        os.remove(q)
    return out


def shot_args(rest):
    names = [x for x in rest if not x.startswith("--") and "," not in x]
    crop = None
    if "--crop" in rest:
        crop = tuple(int(v) for v in rest[rest.index("--crop") + 1].split(","))
    if "--minimap" in rest:
        crop = (1500, 730, 420, 340)
    return (names[0] if names else None), "--full" in rest, crop


def main():
    a = sys.argv[1:]
    if len(a) < 2:
        sys.exit(__doc__)
    root, cmd, rest = root_of(a[0]), a[1], a[2:]
    if cmd == "frame":
        print(frame(root))
    elif cmd == "alive":
        sys.exit(0 if alive(root) else 1)
    elif cmd == "wait":
        print(wait_frames(root, int(rest[0])))
    elif cmd == "send":
        print(send(root, rest[0], wait="--no-wait" not in rest))
    elif cmd == "play":
        print(play(root, rest[0], int(rest[1]) if len(rest) > 1 else None))
    elif cmd == "sweep":
        print(sweep(root, rest[0], int(rest[1]) if len(rest) > 1 else 9, rest[2] if len(rest) > 2 else None))
    elif cmd == "shot":
        print(shot(root, *shot_args(rest)))
    elif cmd == "go":
        send(root, rest[0])
        print(shot(root, *shot_args(rest[1:])))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
