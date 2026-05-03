#!/usr/bin/env python3
import argparse
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", required=True, dest="socket_path")
    parser.add_argument("--root", required=True, dest="root_dir")
    return parser.parse_args()


class HostBridge:
    def __init__(self, socket_path: str, root_dir: str):
        self.socket_path = socket_path
        self.root_dir = Path(root_dir)
        self.sock = None
        self.sock_lock = threading.Lock()
        self.active = {}

    def connect(self):
        deadline = time.time() + 15.0
        while time.time() < deadline:
            try:
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sock.connect(self.socket_path)
                self.sock = sock
                return True
            except OSError:
                time.sleep(0.1)
        return False

    def send_line(self, line: str):
        if not self.sock:
            return
        data = (line + "\n").encode("utf-8", "replace")
        with self.sock_lock:
            try:
                self.sock.sendall(data)
            except OSError:
                pass

    def launch_nds(self, request_id: str, rom_name: str):
        if request_id in self.active:
            return

        def worker():
            script = self.root_dir / "tools" / "launch_nds_host.sh"
            cmd = ["bash", str(script), str(self.root_dir), request_id, rom_name]
            proc = None
            try:
                proc = subprocess.Popen(
                    cmd,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.PIPE,
                    text=True,
                )
                time.sleep(0.35)
                rc = proc.poll()
                if rc is not None and rc != 0:
                    err = ""
                    if proc.stderr:
                        err = proc.stderr.read().strip()
                    if not err:
                        err = f"launch rc={rc}"
                    self.send_line(f"ALOS_HOSTEVENT|NDS|{request_id}|ERROR|{err}")
                    return

                self.send_line(f"ALOS_HOSTEVENT|NDS|{request_id}|STARTED|{rom_name}")
                rc = proc.wait()
                if rc == 0:
                    self.send_line(f"ALOS_HOSTEVENT|NDS|{request_id}|CLOSED|{rom_name}")
                else:
                    err = ""
                    if proc.stderr:
                        err = proc.stderr.read().strip()
                    if not err:
                        err = f"viewer rc={rc}"
                    self.send_line(f"ALOS_HOSTEVENT|NDS|{request_id}|ERROR|{err}")
            except Exception as exc:
                self.send_line(f"ALOS_HOSTEVENT|NDS|{request_id}|ERROR|{exc}")
            finally:
                self.active.pop(request_id, None)
                if proc and proc.stderr:
                    proc.stderr.close()

        thread = threading.Thread(target=worker, daemon=True)
        self.active[request_id] = thread
        thread.start()

    def run(self):
        if not self.connect():
            print("[serial_host_bridge] impossible de se connecter au socket QEMU", file=sys.stderr)
            return 1

        buf = ""
        while True:
            try:
                chunk = self.sock.recv(4096)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk.decode("utf-8", "replace")
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                line = line.replace("\r", "")
                if line == "ALOS_HOSTPING|NDS":
                    self.send_line("ALOS_HOSTEVENT|NDS|0|READY|host-bridge")
                    continue
                if line.startswith("ALOS_HOSTCMD|NDS|"):
                    parts = line.split("|", 3)
                    if len(parts) >= 4 and parts[2] and parts[3]:
                        self.launch_nds(parts[2], parts[3])
        return 0


def main():
    args = parse_args()
    bridge = HostBridge(args.socket_path, args.root_dir)
    raise SystemExit(bridge.run())


if __name__ == "__main__":
    main()
