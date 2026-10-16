# 4agent

A Linux terminal UI for temporary AI agent accounts, written in C++20. Create a dedicated account with ordinary user permissions or unrestricted passwordless sudo, choose a lifetime, inspect interactive Bash history, and revoke accounts immediately.

## Install with APT

Ubuntu 22.04+ / Debian 12+ on amd64, with systemd:

```sh
echo "deb [arch=amd64 trusted=yes] https://hdmain.github.io/4agent ./" \
  | sudo tee /etc/apt/sources.list.d/4agent.list
sudo apt update
sudo apt install 4agent
sudo 4agent
```

This follows the flat GitHub Pages repository setup used by [mdsys](https://github.com/hdmain/mdsys). The package installs `/usr/sbin/4agent`, its runtime dependencies, and the expiration timer. The timer is enabled automatically on systems running systemd. Updates are delivered by `sudo apt update && sudo apt upgrade`. The default export is `/var/lib/4agent/exports/4agent-(hostname).txt` with root-only permissions.

Before removing the package, revoke its active accounts with `sudo 4agent --revoke USERNAME`; package removal is refused while managed accounts remain. Exported credentials and archived history are preserved on removal. If migrating from a manual `/usr/local` installation, remove its executable and unit-file overrides after installing the package so the packaged executable and timer are used. Existing authorized keys pointing at `/usr/local/sbin/4agent` need a compatibility symlink to `/usr/sbin/4agent` until those accounts expire.

Every push to `main` builds and tests the Debian package, then publishes the APT repository to GitHub Pages. Version tags also publish the `.deb` as a GitHub release asset. Workflow runs and Debian package revisions are linked by their run number.

To build a package locally:

```sh
sudo apt install build-essential cmake libncurses-dev dpkg-dev apt-utils
bash scripts/build-deb.sh /var/tmp/4agent-deb 1.0.0-1
sudo apt install /var/tmp/4agent-deb/4agent_1.0.0-1_amd64.deb
```

## Build and install

Ubuntu / Debian (including Ubuntu in WSL):

```sh
sudo apt-get update
sudo apt-get install -y build-essential cmake libncurses-dev openssh-server openssh-client sudo curl procps python3
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
sudo cmake --install build
sudo systemctl daemon-reload
sudo systemctl enable --now 4agent-expire.timer
sudo systemctl enable --now ssh
sudo /usr/local/sbin/4agent
```

The default install prefix is `/usr/local`. If your sudo PATH omits `/usr/local/sbin`, use the full path shown above. A custom `CMAKE_INSTALL_PREFIX` is supported: the service uses the configured absolute executable path. Make its `lib/systemd/system` directory discoverable to systemd before enabling the timer. Linux with systemd is required; the tool refuses account creation if its expiration timer is inactive.

For WSL, enable systemd in `/etc/wsl.conf` if needed:

```ini
[boot]
systemd=true
```

Restart WSL with `wsl --shutdown` from Windows, then build inside your Ubuntu distribution. Prefer a Linux filesystem build directory, for example `/var/tmp/4agent-build`, when the source is on `/mnt/c`.

## Use

Run `sudo 4agent` for the terminal UI. Choose **Create account**, enter a name beginning with `agent_`, a lifetime in minutes, permission level, public IP (or auto-detect), SSH port, and export directory. The full-screen ncurses interface supports arrow keys, Tab between panels and form fields, Enter to activate, and Esc to cancel. Use N to create, H for history, D to revoke, R to refresh, and Q to quit. Account lists refresh automatically; history supports scrolling and Page Up/Down. The terminal must be at least 80 columns by 24 rows.

The port is your **externally reachable SSH port**; it defaults to 22. 4agent does not change sshd, firewall, router, or WSL forwarding settings. When using NAT, configure those separately. OpenSSH must allow public-key authentication and the selected user. Root permissions are provided by `sudo -i` after signing in as the temporary user; direct SSH login as root is unnecessary.

The export is `4agent-(hostname).txt` in the chosen directory. By default, the TUI and CLI use `/var/lib/4agent/exports`, an automatically created root-only Linux directory, independent of the current working directory. Launching from a Windows-mounted WSL project folder therefore works. Each new account appends its own section, containing:

- Username, permissions, and expiration in UTC.
- Ed25519 public and private keys.
- Public IP, SSH port, and step-by-step connection instructions.

The export is root-owned with mode `0600`; existing exports must have those permissions. The export destination is validated before provisioning an account or generating keys. Use a Linux filesystem for custom exports: Windows-mounted directories may not enforce Linux file permissions. The private keys are unencrypted so agents can use them unattended. Transfer the export securely, copy the private key block into a client key file, run `chmod 600` on that file, and follow the exported SSH command. Verify the server host key through a trusted channel. Delete exports when no longer needed; account deletion does not erase exported credentials.

Public IP detection queries `api.ipify.org`, then `ifconfig.me`, over HTTPS with short timeouts. Supply the address explicitly to avoid those requests. IPv4 and IPv6 are supported. A detected public IP does not establish inbound connectivity, especially on WSL.

The CLI supports scripts and administration:

```sh
sudo 4agent --create --name agent_demo --minutes 60 --public-ip 203.0.113.10 --port 22 --output /root
sudo 4agent --create --name agent_admin --minutes 30 --root --public-ip 203.0.113.10 --output /root
sudo 4agent --list
sudo 4agent --history agent_demo
sudo 4agent --revoke agent_demo
sudo 4agent --reap
4agent --help
```

`203.0.113.10` is an example; replace it with your server's address. Names allow 1–24 lowercase letters, digits, or underscores after `agent_`. Lifetime is 1–525600 minutes. Non-root access is the default. Accounts have no usable password. SSH keys disable forwarding while allowing a terminal session.

## Expiration and history

Generated authorized keys include OpenSSH's `expiry-time` timestamp. A systemd timer checks every 15 seconds (one-second scheduling accuracy), locks expired accounts, removes their authorized keys, terminates processes owned by their UID, and deletes the account and its home. The timer starts after boot; key expiry also prevents use of the exported key while cleanup is pending. Scheduling delays or an unavailable systemd service can delay cleanup. Failed removals are retried; inspect `journalctl -u 4agent-expire.service` for failures. Account expiry dates also provide a day-granularity login backstop.

State and archived history live in root-only `/var/lib/4agent`. The SSH session helper records noninteractive SSH command requests, and interactive Bash appends completed commands to `.bash_history` after each prompt; the viewer displays it safely as text. On revocation it is archived, if available, so `--history` also works after removal. Archived names cannot be reused. History can contain secrets. This is **user-editable shell history, not an audit log**: commands inside other shells, interactive commands that have not returned to the prompt, and commands in a root shell are not recorded. A noninteractive request is logged as the whole requested shell string, rather than every subprocess it runs. The session helper is enforced by generated authorized-key options; replacing those options can bypass it. The OpenSSH `internal-sftp` subsystem is not supported by this command wrapper; use the external `sftp-server` subsystem if file transfers are needed. Existing `.bashrc` configuration or user changes can also disable recording.

An account with unrestricted sudo has full control over the server: it can disable cleanup, alter history, or create other access. Root processes it creates are outside UID-based cleanup. Ordinary accounts are Linux users, not containers or security sandboxes; they can access resources available to other local users. Temporary lifetime does not undo filesystem changes or network actions.

## WSL tests

```sh
cmake -S . -B /var/tmp/4agent-build -DCMAKE_BUILD_TYPE=Debug
cmake --build /var/tmp/4agent-build -j
ctest --test-dir /var/tmp/4agent-build --output-on-failure
sudo cmake --install /var/tmp/4agent-build
sudo systemctl daemon-reload
sudo systemctl enable --now 4agent-expire.timer
sudo bash tests/integration.sh /usr/local/sbin/4agent
sudo apt-get install -y python3-pyte
sudo python3 tests/tui_tests.py /usr/local/sbin/4agent
```

Integration tests create uniquely named disposable accounts, check keys, export permissions, sudo, history, rollback, active-process termination, and timer-driven expiration, then clean up their own accounts and reports. Run them in a disposable WSL distribution or test VM. If `sshd` is installed, they additionally verify real SSH login with the exported key and denial after revocation using an isolated localhost listener.
