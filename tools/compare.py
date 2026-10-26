"""Runs a stretch of a game on the runtime and on Zakuro, with the same
recorded input, and reports where they part: the screens every so many
frames, the service commands each was asked (counted as ZAKURO_USAGE counts
them) and the save files at the end. Zakuro already runs the games, so a
difference points at the runtime.

    python -I tools/compare.py <exheader.bin> <code.bin> <library> <game .3ds> <update .cia>
        <user folder to copy> <recording> <out folder> [--frames N] [--every N] [--zakuro exe]

Needs Pillow. The user folder is copied for each run, never written.
"""

import argparse
import filecmp
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
RUNTIME = HERE.parent
BOOT = RUNTIME / "build" / "boot.exe"
ZAKURO = RUNTIME.parent / "zakuro-dev" / "target" / "release" / "zakuro.exe"


def usage(path):
    """the commands a run was asked, (service, command) to how many times"""
    counts = {}
    if not path.exists():
        return counts
    for line in path.read_text().splitlines():
        fields = line.split()
        if len(fields) >= 4 and fields[1] == "ipc":
            counts[(fields[2], int(fields[3], 16))] = int(fields[0])
    return counts


def run_runtime(args, out):
    data = out / "rt-data"
    shutil.copytree(args.user, data / "user")
    shots = out / "rt-shots"
    shots.mkdir()
    env = dict(os.environ, RUNTIME_FRAMES=str(args.frames), RUNTIME_REPLAY=str(args.recording),
               RUNTIME_SHOTS=str(shots), RUNTIME_SHOT_EVERY=str(args.every),
               RUNTIME_USAGE=str(out / "rt-usage.txt"))
    start = time.time()
    with open(out / "rt.log", "w") as log:
        subprocess.run([str(BOOT), str(args.exheader), str(args.code), str(args.library), "0", str(args.game),
                        str(args.update), str(data / "user")], env=env, stdout=log, stderr=subprocess.STDOUT,
                       cwd=out)
    return time.time() - start


def run_zakuro(args, out):
    data = out / "zk-data"
    shutil.copytree(args.user, data / "user")
    shots = out / "zk-shots"
    shots.mkdir()
    env = dict(os.environ, ZAKURO_REPLAY=str(args.recording), ZAKURO_USAGE=str(out / "zk-usage.txt"))
    start = time.time()
    with open(out / "zk.log", "w") as log:
        # the update is taken from the game's folder
        subprocess.run([str(args.zakuro), str(args.game), "--headless", str(args.frames), "--mute", "--data",
                        str(data), "--screenshots", str(shots), "--screenshot-every", str(args.every)],
                       env=env, stdout=log, stderr=subprocess.STDOUT, cwd=out)
    return time.time() - start


def compare_screens(out, frames, every, threshold):
    from PIL import Image, ImageChops, ImageStat

    parted = []
    for frame in range(every, frames + 1, every):
        for screen in ("top", "bottom"):
            z = out / "zk-shots" / f"{frame:06d}-{screen}.png"
            r = out / "rt-shots" / f"{frame:06d}-{screen}.ppm"
            if not z.exists() or not r.exists():
                parted.append((frame, screen, None))
                continue
            a = Image.open(z).convert("RGB")
            b = Image.open(r).convert("RGB")
            # Zakuro draws at its scale, the runtime at the 3DS's
            a = a.resize(b.size, Image.BILINEAR)
            difference = sum(ImageStat.Stat(ImageChops.difference(a, b)).mean) / 3
            if difference > threshold:
                parted.append((frame, screen, difference))
    return parted


def compare_files(a, b):
    """files that differ or are only on one side, by path under both"""
    found = []
    for path in sorted(set(p.relative_to(a) for p in a.rglob("*") if p.is_file()) |
                       set(p.relative_to(b) for p in b.rglob("*") if p.is_file())):
        x, y = a / path, b / path
        if not x.exists():
            found.append(f"only the runtime wrote {path}")
        elif not y.exists():
            found.append(f"only Zakuro wrote {path}")
        elif not filecmp.cmp(x, y, shallow=False):
            found.append(f"differs: {path}")
    return found


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ("exheader", "code", "library", "game", "update", "user", "recording", "out"):
        p.add_argument(name, type=Path)
    p.add_argument("--frames", type=int, default=3000)
    p.add_argument("--every", type=int, default=300)
    p.add_argument("--zakuro", type=Path, default=ZAKURO)
    p.add_argument("--threshold", type=float, default=8.0, help="mean difference a screen may have, 0-255")
    args = p.parse_args()

    out = args.out
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    rt = run_runtime(args, out)
    zk = run_zakuro(args, out)
    print(f"{args.frames} frames: runtime {rt:.1f} s, Zakuro {zk:.1f} s")

    problems = 0
    for frame, screen, difference in compare_screens(out, args.frames, args.every, args.threshold):
        problems += 1
        print(f"screen {screen} at frame {frame}: " + ("missing" if difference is None else f"differs by {difference:.1f}"))

    z, r = usage(out / "zk-usage.txt"), usage(out / "rt-usage.txt")
    for key in sorted(set(z) | set(r)):
        a, b = z.get(key, 0), r.get(key, 0)
        # timing moves counts a little; a command only one side got, or far apart, is a lead
        if a == 0 or b == 0 or (max(a, b) > 10 and abs(a - b) > max(a, b) * 0.25):
            problems += 1
            print(f"{key[0]} 0x{key[1]:04X}: Zakuro {a}, runtime {b}")

    for line in compare_files(out / "zk-data" / "user", out / "rt-data" / "user"):
        problems += 1
        print(line)
    for line in (out / "rt.log").read_text(errors="replace").splitlines():
        if line.startswith(("unanswered", "error", "fault")):
            print("runtime:", line)
    print("no differences" if not problems else f"{problems} differences")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
