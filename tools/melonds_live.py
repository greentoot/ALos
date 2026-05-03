import argparse
import shlex
import struct
import subprocess
import sys
import tkinter as tk
from pathlib import Path

from PIL import Image, ImageTk


FRAME_MAGIC = 0x414C4652
INPUT_MAGIC = 0x414C494E
FRAME_HEADER = struct.Struct("<6I")
INPUT_PACKET = struct.Struct("<7I")
SCREEN_W = 256
SCREEN_H = 384
PIXEL_BYTES = SCREEN_W * SCREEN_H * 4
DEFAULT_KEYS = 0x0FFF

KEY_BITS = {
    "z": 0,
    "x": 1,
    "BackSpace": 2,
    "Return": 3,
    "Right": 4,
    "Left": 5,
    "Up": 6,
    "Down": 7,
    "s": 8,
    "a": 9,
    "q": 10,
    "w": 11,
}


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", required=True)
    parser.add_argument("--project-root-wsl", required=True)
    parser.add_argument("--rom", required=True)
    parser.add_argument("--rom-wsl", required=True)
    parser.add_argument("--runner-wsl", required=True)
    parser.add_argument("--ipc-dir", required=True)
    parser.add_argument("--ipc-dir-wsl", required=True)
    parser.add_argument("--scale", type=int, default=2)
    return parser.parse_args()


class MelonDSLiveViewer:
    def __init__(self, args):
        self.args = args
        self.ipc_dir = Path(args.ipc_dir)
        self.frame_path = self.ipc_dir / "frame.bin"
        self.input_path = self.ipc_dir / "input.bin"
        self.scale = max(1, args.scale)
        self.pressed = set()
        self.touch_active = False
        self.touch_x = 0
        self.touch_y = 0
        self.last_frame_id = -1
        self.photo = None
        self.stop_requested = False

        self.ipc_dir.mkdir(parents=True, exist_ok=True)
        self._write_input()
        self.proc = self._spawn_runner()

        self.root = tk.Tk()
        self.root.title(f"ALOS DS - {Path(args.rom).stem}")
        self.root.configure(bg="#141414")
        self.root.protocol("WM_DELETE_WINDOW", self.close)

        self.status_var = tk.StringVar(value="Chargement Nintendo DS...")
        self.label = tk.Label(self.root, bg="#141414", bd=0, highlightthickness=0)
        self.label.pack(padx=12, pady=(12, 6))
        self.status = tk.Label(self.root, textvariable=self.status_var, fg="#e0e0e0", bg="#141414")
        self.status.pack(padx=12, pady=(0, 12))

        self.root.bind("<KeyPress>", self.on_key_press)
        self.root.bind("<KeyRelease>", self.on_key_release)
        self.label.bind("<ButtonPress-1>", self.on_touch_press)
        self.label.bind("<B1-Motion>", self.on_touch_drag)
        self.label.bind("<ButtonRelease-1>", self.on_touch_release)
        self.label.focus_set()
        self.root.after(16, self.tick)

    def _spawn_runner(self):
        cmd = (
            f"cd {shlex.quote(self.args.project_root_wsl)} && "
            f"{shlex.quote(self.args.runner_wsl)} "
            f"{shlex.quote(self.args.rom_wsl)} "
            f"--project-root {shlex.quote(self.args.project_root_wsl)} "
            f"--live --ipc-dir {shlex.quote(self.args.ipc_dir_wsl)}"
        )
        return subprocess.Popen(
            ["wsl.exe", "bash", "-lc", cmd],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

    def _key_mask(self):
        mask = DEFAULT_KEYS
        for bit in self.pressed:
            mask &= ~(1 << bit)
        return mask

    def _write_input(self):
        data = INPUT_PACKET.pack(
            INPUT_MAGIC,
            1,
            self._key_mask(),
            1 if self.touch_active else 0,
            self.touch_x,
            self.touch_y,
            1 if self.stop_requested else 0,
        )
        self.input_path.write_bytes(data)

    def on_key_press(self, event):
        if event.keysym == "Escape":
            self.close()
            return
        bit = KEY_BITS.get(event.keysym)
        if bit is not None:
            self.pressed.add(bit)
            self._write_input()

    def on_key_release(self, event):
        bit = KEY_BITS.get(event.keysym)
        if bit is not None and bit in self.pressed:
            self.pressed.remove(bit)
            self._write_input()

    def _update_touch(self, event):
        x = max(0, min(SCREEN_W - 1, event.x // self.scale))
        y = max(0, min(SCREEN_H - 1, event.y // self.scale))
        if y < 192:
            self.touch_active = False
        else:
            self.touch_active = True
            self.touch_x = x
            self.touch_y = y - 192
        self._write_input()

    def on_touch_press(self, event):
        self._update_touch(event)

    def on_touch_drag(self, event):
        self._update_touch(event)

    def on_touch_release(self, _event):
        self.touch_active = False
        self._write_input()

    def _load_frame(self):
        if not self.frame_path.exists():
            return None
        raw = self.frame_path.read_bytes()
        if len(raw) < FRAME_HEADER.size + PIXEL_BYTES:
            return None
        magic, version, width, height, frame_id, flags = FRAME_HEADER.unpack_from(raw, 0)
        if magic != FRAME_MAGIC or version != 1 or width != SCREEN_W or height != SCREEN_H:
            return None
        if frame_id == self.last_frame_id:
            return flags, None
        self.last_frame_id = frame_id
        payload = raw[FRAME_HEADER.size:FRAME_HEADER.size + PIXEL_BYTES]
        image = Image.frombytes("RGBA", (SCREEN_W, SCREEN_H), payload)
        if self.scale != 1:
            image = image.resize((SCREEN_W * self.scale, SCREEN_H * self.scale), Image.Resampling.NEAREST)
        return flags, image

    def tick(self):
        self._write_input()
        result = self._load_frame()
        if result is not None:
            flags, image = result
            if image is not None:
                self.photo = ImageTk.PhotoImage(image)
                self.label.configure(image=self.photo)
            self.status_var.set("Fleches/Z/X/Entree/Backspace/A/S/Q/W + souris sur ecran bas")
            if flags == 0 and self.proc.poll() is not None:
                self.status_var.set("Session DS terminee. Ferme la fenetre pour revenir a ALOS.")
        elif self.proc.poll() is not None:
            self.status_var.set("Le runner DS s'est arrete avant de produire une frame.")
        self.root.after(16, self.tick)

    def close(self):
        if self.stop_requested:
            return
        self.stop_requested = True
        self.touch_active = False
        self._write_input()
        try:
            self.proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.proc.terminate()
        self.root.destroy()


def main():
    args = parse_args()
    viewer = MelonDSLiveViewer(args)
    viewer.root.mainloop()


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"[melonds_live] {exc}", file=sys.stderr)
        raise
