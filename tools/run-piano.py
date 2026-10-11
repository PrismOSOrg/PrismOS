#!/usr/bin/env python3
import shutil
import shlex
import socket
import subprocess
import sys
import time
from pathlib import Path


DISK_SIZE = 64 * 1024 * 1024
PARTITION_OFFSET = 2048 * 512
SERIAL_READY = "shell_run entered"
COMPILE_DONE = "in-OS prismcc compile completed"
COMPILE_FAILED = "in-OS prismcc compile failed"
DRIVER_READY = "resident driver initialization completed"
APP_RUNNING = "app runtime dispatching BCVM app"


def run_checked(arguments, **kwargs):
    subprocess.run(arguments, check=True, **kwargs)


def require_tools():
    tools = ("sfdisk", "mkfs.fat", "mcopy", "mmd")
    missing = [tool for tool in tools if shutil.which(tool) is None]
    if missing:
        raise RuntimeError("Missing required host tools: " + ", ".join(missing))


def prepare_disk(disk, root):
    disk.parent.mkdir(parents=True, exist_ok=True)
    with disk.open("wb") as image:
        image.truncate(DISK_SIZE)

    run_checked(["sfdisk", str(disk)], input=(
        "label: dos\nunit: sectors\n\n"
        "start=2048, size=129024, type=c, bootable\n"
    ), text=True, stdout=subprocess.DEVNULL)
    run_checked([
        "mkfs.fat", "-F", "32", "-n", "PIANO",
        "--offset=2048", str(disk),
    ], stdout=subprocess.DEVNULL)

    image_spec = f"{disk}@@{PARTITION_OFFSET}"
    run_checked(["mmd", "-i", image_spec, "::/drivers"])
    run_checked(["mmd", "-i", image_spec, "::/apps"])
    run_checked([
        "mcopy", "-i", image_spec, str(root / "examples/ac97_prismcc.c"),
        "::/ac97.c",
    ])
    run_checked([
        "mcopy", "-i", image_spec, str(root / "examples/piano_prismcc.c"),
        "::/piano.c",
    ])


def wait_for_log(log_path, marker, timeout, process, occurrences=1):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"QEMU exited with status {process.returncode}")
        try:
            log = log_path.read_text(errors="replace")
            if marker == COMPILE_DONE and COMPILE_FAILED in log:
                raise RuntimeError(f"PrismCC compilation failed.\n{log}")
            if log.count(marker) >= occurrences:
                return
        except FileNotFoundError:
            pass
        time.sleep(0.1)
    tail = ""
    try:
        tail = "\n".join(log_path.read_text(errors="replace").splitlines()[-30:])
    except FileNotFoundError:
        pass
    raise RuntimeError(f"Timed out waiting for {marker!r} in serial output.\n{tail}")


def monitor_connect(path, process):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"QEMU exited with status {process.returncode}")
        try:
            monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            monitor.connect(str(path))
            monitor.settimeout(1)
            return monitor
        except OSError:
            time.sleep(0.1)
    raise RuntimeError("Could not connect to the QEMU monitor")


def send_command(monitor, command):
    keys = {" ": "spc", "/": "slash", ".": "dot", "-": "minus"}
    for character in command:
        key = keys.get(character, character)
        monitor.sendall(f"sendkey {key}\n".encode())
        time.sleep(0.08)
    monitor.sendall(b"sendkey ret\n")


def main():
    if len(sys.argv) != 5:
        raise RuntimeError("Usage: run-piano.py QEMU ISO DISK AUDIO_BACKEND")

    qemu, iso, disk, audio_backend = sys.argv[1:]
    root = Path(__file__).resolve().parent.parent
    disk = Path(disk).resolve()
    serial_log = disk.with_suffix(".serial.log")
    monitor_path = disk.with_suffix(".monitor.sock")
    require_tools()
    prepare_disk(disk, root)
    serial_log.write_text("")
    monitor_path.unlink(missing_ok=True)

    qemu_arguments = shlex.split(qemu) + [
        "-m", "128M", "-boot", "order=d", "-cdrom", iso,
        "-drive", f"file={disk},format=raw,if=ide,index=0,media=disk",
        "-monitor", f"unix:{monitor_path},server=on,wait=off",
        "-serial", f"file:{serial_log}",
        "-audiodev", f"{audio_backend},id=audio0",
        "-device", "AC97,audiodev=audio0",
    ]
    process = subprocess.Popen(qemu_arguments)
    try:
        monitor = monitor_connect(monitor_path, process)
        wait_for_log(serial_log, SERIAL_READY, 45, process)

        send_command(monitor, "cc /ac97.c /drivers/ac97.pdr")
        wait_for_log(serial_log, COMPILE_DONE, 90, process)
        send_command(monitor, "cc /piano.c /apps/piano.app")
        wait_for_log(serial_log, COMPILE_DONE, 90, process, occurrences=2)
        send_command(monitor, "driver-start /drivers/ac97.pdr")
        wait_for_log(serial_log, DRIVER_READY, 45, process)
        send_command(monitor, "app-run /apps/piano.app")
        wait_for_log(serial_log, APP_RUNNING, 45, process)
        monitor.close()
        print("Piano is running. Press Escape in QEMU to return to the shell; close QEMU to exit.")
        return process.wait()
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait()
        monitor_path.unlink(missing_ok=True)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"run-piano: {error}", file=sys.stderr)
        sys.exit(1)
