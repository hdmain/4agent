"""Real PTY + terminal-emulator tests. Requires root and python3-pyte in test WSL."""
import fcntl
import os
import pathlib
import pty
import pwd
import re
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import time

import pyte

if os.geteuid() != 0:
    raise SystemExit("Run as root inside test WSL")
binary = sys.argv[1] if len(sys.argv) > 1 else "/usr/local/sbin/4agent"
name = f"agent_ui_{os.getpid()}"
host = re.sub(r"[^a-zA-Z0-9.-]", "_", socket.gethostname())
export = pathlib.Path("/var/lib/4agent/exports") / f"4agent-{host}.txt"
master, slave = pty.openpty()
fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 32, 110, 0, 0))
screen = pyte.Screen(110, 32)
stream = pyte.ByteStream(screen)
ui = subprocess.Popen([binary], stdin=slave, stdout=slave, stderr=slave, close_fds=True, env={**os.environ, "TERM": "xterm-256color"})
os.close(slave)


def wait_for(value, timeout=15):
    deadline = time.monotonic() + timeout
    last_data = time.monotonic()
    while time.monotonic() < deadline:
        if select.select([master], [], [], 0.1)[0]:
            try:
                stream.feed(os.read(master, 65536))
                last_data = time.monotonic()
            except OSError:
                raise AssertionError(f"TUI exited before displaying {value!r}")
        display = "\n".join(screen.display)
        if value in display and time.monotonic() - last_data >= 0.1:
            return display
    raise AssertionError(f"TUI did not display {value!r}:\n" + "\n".join(screen.display))


def send(value):
    os.write(master, value)


def exists(user):
    try:
        pwd.getpwnam(user)
        return True
    except KeyError:
        return False


try:
    assert not exists(name)
    wait_for("Agent access manager")
    # ncurses application-mode arrow keys navigate the sidebar.
    send(b"\t\x1bOB\r")
    wait_for("ACCOUNT CONFIGURATION")
    send(b"\x15" + name.encode() + b"\t\x15" + b"1\t \t203.0.113.10\t\t")
    display = wait_for("Root via sudo")
    assert "/var/lib/4agent/exports" in display
    send(b"\t\r")
    wait_for("Account created")
    wait_for(str(export))
    assert exists(name)
    assert export.stat().st_uid == 0 and export.stat().st_mode & 0o777 == 0o600
    assert export.parent.stat().st_mode & 0o777 == 0o700
    assert f"Username: {name}\n" in export.read_text()
    send(b"\r")
    wait_for("Agent access manager")
    # Return focus to the account table, then open the selected account's history.
    send(b"\t")
    account_names = [line.split()[0] for line in subprocess.check_output([binary, "--list"], text=True).splitlines() if line.startswith("agent_")]
    send(b"k" * len(account_names) + b"j" * account_names.index(name))
    wait_for("> " + name)
    subprocess.run(["runuser", "-u", name, "--", "bash", "-ic", "history -s 'echo ui_history_marker'; history -a"], capture_output=True, check=True)
    send(b"h")
    wait_for("ui_history_marker")
    send(b"\x1b")
    wait_for("Agent access manager")
    # Canceling a revoke must preserve the account.
    send(b"d")
    assert name in wait_for("CONFIRM REMOVAL")
    send(b"\r")
    wait_for("Agent access manager")
    assert exists(name)
    # Resize to the supported minimum and verify the form fits without overlapping controls.
    screen.resize(lines=24, columns=80)
    fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    os.kill(ui.pid, signal.SIGWINCH)
    wait_for("Agent access manager")
    send(b"n")
    display = wait_for("ACCOUNT CONFIGURATION")
    assert "Create account" in display and "Esc Cancel" in display
    send(b"\x1b")
    wait_for("Agent access manager")
    send(b"d")
    assert name in wait_for("CONFIRM REMOVAL")
    send(b"\x1bOC\r")
    wait_for(f"Revoked {name}.")
    assert not exists(name)
    send(b"q")
    assert ui.wait(timeout=5) == 0
    print("PASS: rendered dashboard, arrow/Tab navigation, editing, root creation, Linux default export, history, cancel/revoke, resize, clean exit", flush=True)

    with tempfile.TemporaryDirectory(prefix="4agent-export-reject-") as temporary:
        unsafe = pathlib.Path(temporary) / export.name
        unsafe.write_text("existing content")
        unsafe.chmod(0o644)
        rejected = name + "_bad"
        before = set(pathlib.Path("/var/lib/4agent").glob("key-*"))
        result = subprocess.run([binary, "--create", "--name", rejected, "--root", "--public-ip", "203.0.113.10", "--output", temporary], text=True, capture_output=True, timeout=15)
        assert result.returncode and str(unsafe) in result.stderr and "/var/lib/4agent/exports" in result.stderr
        assert unsafe.read_text() == "existing content" and not exists(rejected)
        assert set(pathlib.Path("/var/lib/4agent").glob("key-*")) == before
        print("PASS: insecure export rejected before keys or account creation, with recovery instructions", flush=True)
finally:
    if ui.poll() is None:
        ui.terminate()
        ui.wait(timeout=5)
    os.close(master)
    if exists(name):
        subprocess.run([binary, "--revoke", name], capture_output=True, check=True, timeout=20)
    pathlib.Path(f"/var/lib/4agent/{name}.history").unlink(missing_ok=True)
    if export.exists():
        # Remove this test's credentials while preserving any other account sections.
        fd = os.open(export, os.O_RDWR | os.O_NOFOLLOW)
        with os.fdopen(fd, "r+b") as file:
            fcntl.flock(file, fcntl.LOCK_EX)
            separator = b"\n========== 4agent account ==========\n"
            parts = file.read().split(separator)
            parts = [part for part in parts if f"Username: {name}\n".encode() not in part]
            file.seek(0)
            file.write(separator.join(parts))
            file.truncate()
            file.flush()
            os.fsync(file.fileno())
