"""Destructive only to uniquely named test accounts; run as root in a test VM/WSL."""
import os
import fcntl
import pathlib
import pty
import pwd
import re
import select
import shutil
import socket
import subprocess
import struct
import sys
import tempfile
import time

if os.geteuid() != 0:
    raise SystemExit("Run as root in a disposable WSL distribution or VM")
binary = str(pathlib.Path(sys.argv[1]).resolve())
work = pathlib.Path(tempfile.mkdtemp(prefix="4agent-test-"))
names = [f"agent_t{os.getpid()}_{i}" for i in range(4)]
sshd = None
sleeper = None
created = []
extra_policy = pathlib.Path("/etc/sudoers.d") / ("4agent-" + names[3])


def run(*args, ok=True):
    result = subprocess.run([str(a) for a in args], text=True, capture_output=True, timeout=30)
    if ok and result.returncode:
        raise AssertionError(f"Command failed: {args[0:2]}: {result.stderr}")
    return result


def exists(name):
    try:
        pwd.getpwnam(name)
        return True
    except KeyError:
        return False


def create(name, *extra, output=work):
    result = run(binary, "--create", "--name", name, "--minutes", "1", "--public-ip", "203.0.113.10", "--output", output, *extra)
    created.append(name)
    return result


try:
    assert not any(exists(name) for name in names)
    run(binary, "--help")
    assert run(binary, "--create", "--name", "root", "--public-ip", "203.0.113.10", ok=False).returncode
    assert run(binary, "--create", "--name", names[0], "--minutes", "0", ok=False).returncode
    assert run("runuser", "-u", "nobody", "--", binary, "--list", ok=False).returncode
    create(names[0])
    create(names[1], "--root")
    export = next(work.glob("4agent-*.txt"))
    assert export.stat().st_mode & 0o777 == 0o600
    assert export.stat().st_uid == 0
    report = export.read_text()
    assert report.count("========== 4agent account ==========") == 2
    assert "203.0.113.10" in report and "SSH port: 22" in report
    keys = re.findall(r"-----BEGIN OPENSSH PRIVATE KEY-----.*?-----END OPENSSH PRIVATE KEY-----", report, re.S)
    assert len(keys) == 2
    client_keys = []
    for i, key in enumerate(keys):
        path = work / f"client{i}"
        path.write_text(key + "\n")
        path.chmod(0o600)
        public = run("ssh-keygen", "-y", "-f", path).stdout.split()[:2]
        authorized = pathlib.Path(f"/home/{names[i]}/.ssh/authorized_keys").read_text()
        assert " ".join(public) in authorized and 'expiry-time="' in authorized
        client_keys.append(path)
    assert run("runuser", "-u", names[0], "--", "sudo", "-n", "id", "-u", ok=False).returncode
    assert run("runuser", "-u", names[1], "--", "sudo", "-n", "id", "-u").stdout.strip() == "0"
    assert all(name in run(binary, "--list").stdout for name in names[:2])
    assert run(binary, "--create", "--name", names[0], "--public-ip", "203.0.113.10", ok=False).returncode
    run("runuser", "-u", names[0], "--", "bash", "-ic", "history -s 'echo 4agent_history_marker'; history -a")
    assert "4agent_history_marker" in run(binary, "--history", names[0]).stdout
    # Reject symlink exports and roll back the account without changing the target.
    hostile = work / "hostile"
    hostile.mkdir()
    sentinel = work / "sentinel"
    sentinel.write_text("untouched")
    (hostile / export.name).symlink_to(sentinel)
    failed = run(binary, "--create", "--name", names[2], "--public-ip", "203.0.113.10", "--output", hostile, ok=False)
    assert failed.returncode and not exists(names[2]) and sentinel.read_text() == "untouched"
    # A failed sudo policy install must not delete an existing policy.
    assert not extra_policy.exists()
    extra_policy.write_text("# preexisting test policy\n")
    extra_policy.chmod(0o440)
    failed = run(binary, "--create", "--name", names[3], "--root", "--public-ip", "203.0.113.10", "--output", work, ok=False)
    assert failed.returncode and not exists(names[3]) and extra_policy.read_text() == "# preexisting test policy\n"
    extra_policy.unlink()
    print("PASS: creation, permissions, sudo, exports, keys, history, validation and rollback", flush=True)

    # Verify the terminal UI opens and handles a menu choice through an actual PTY.
    master, slave = pty.openpty()
    fcntl.ioctl(slave, __import__("termios").TIOCSWINSZ, struct.pack("HHHH", 30, 110, 0, 0))
    ui = subprocess.Popen([binary], stdin=slave, stdout=slave, stderr=slave, close_fds=True, env={**os.environ, "TERM": "xterm-256color"})
    os.close(slave)
    data = b""
    deadline = time.monotonic() + 5
    while b"Agent access manager" not in data and time.monotonic() < deadline:
        if select.select([master], [], [], 0.5)[0]:
            data += os.read(master, 8192)
    assert b"Agent access manager" in data
    os.write(master, b"q")
    assert ui.wait(timeout=5) == 0
    os.close(master)
    print("PASS: TUI rendering and input through a terminal", flush=True)

    ssh_server = shutil.which("sshd")
    if ssh_server:
        pathlib.Path("/run/sshd").mkdir(mode=0o755, exist_ok=True)
        run("ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", work / "host_key")
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        config = work / "sshd_config"
        config.write_text(f"Port {port}\nListenAddress 127.0.0.1\nHostKey {work}/host_key\nPidFile {work}/sshd.pid\nPasswordAuthentication no\nKbdInteractiveAuthentication no\nUsePAM no\nAllowUsers {names[0]} {names[1]}\nLogLevel ERROR\n")
        run(ssh_server, "-t", "-f", config)
        sshd = subprocess.Popen([ssh_server, "-D", "-e", "-f", str(config)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(50):
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                    break
            except OSError:
                time.sleep(0.1)
        ssh_command = ["ssh", "-o", "BatchMode=yes", "-o", "IdentitiesOnly=yes", "-o", "StrictHostKeyChecking=no", "-o", f"UserKnownHostsFile={work}/known_hosts", "-o", "ConnectTimeout=3", "-i", str(client_keys[1]), "-p", str(port), f"{names[1]}@127.0.0.1"]
        assert run(*ssh_command, "id -un").stdout.strip() == names[1]
        # The unprivileged session wrapper must preserve the SSH user's tool PATH.
        assert "/usr/local/bin" in run(*ssh_command, 'printf "%s" "$PATH"').stdout.split(":")
        assert run(*ssh_command, "sudo -n id -u").stdout.strip() == "0"
        assert "sudo -n id -u" in run(binary, "--history", names[1]).stdout
        run(binary, "--revoke", names[1])
        assert run(*ssh_command, "true", ok=False).returncode
        assert "sudo -n id -u" in run(binary, "--history", names[1]).stdout
        print("PASS: real SSH login, sudo over SSH, and denial after revocation", flush=True)
    else:
        print("SKIP: SSH round trip (openssh-server not installed)", flush=True)
        run(binary, "--revoke", names[1])
    assert not exists(names[1]) and not pathlib.Path(f"/home/{names[1]}").exists()
    assert not pathlib.Path(f"/etc/sudoers.d/4agent-{names[1]}").exists()
    uid = pwd.getpwnam(names[0]).pw_uid
    sleeper = subprocess.Popen(["runuser", "-u", names[0], "--", "sleep", "180"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print("Waiting for the systemd timer to expire the one-minute account...", flush=True)
    deadline = time.monotonic() + 90
    while exists(names[0]) and time.monotonic() < deadline:
        time.sleep(1)
    assert not exists(names[0]), "Timer did not remove expired account"
    sleeper.wait(timeout=5)
    assert run("pgrep", "-u", str(uid), ok=False).returncode == 1
    assert not pathlib.Path(f"/home/{names[0]}").exists()
    assert "4agent_history_marker" in run(binary, "--history", names[0]).stdout
    print("PASS: automatic timer expiration, active process termination, home removal, archived history", flush=True)
finally:
    if sshd:
        sshd.terminate()
        sshd.wait(timeout=5)
    if sleeper and sleeper.poll() is None:
        sleeper.terminate()
    for name in created:
        if exists(name):
            run(binary, "--revoke", name, ok=False)
        if not exists(name):
            pathlib.Path(f"/var/lib/4agent/{name}.history").unlink(missing_ok=True)
    if extra_policy.exists() and extra_policy.read_text() == "# preexisting test policy\n":
        extra_policy.unlink()
    shutil.rmtree(work)
