#!/usr/bin/env bash
set -euo pipefail
if [ "$#" -lt 2 ]; then
    echo "Usage: bash scripts/build-apt-repo.sh OUTPUT_DIRECTORY PACKAGE.deb [...]" >&2
    exit 1
fi
repo_dir=$1
shift
mkdir -p "$repo_dir/pool/main"
architectures=()
for package in "$@"; do
    test "$(dpkg-deb -f "$package" Package)" = 4agent
    cp -- "$package" "$repo_dir/pool/main/"
    architectures+=("$(dpkg-deb -f "$package" Architecture)")
done
cd -- "$repo_dir"
dpkg-scanpackages --multiversion pool/ > Packages
gzip -n -9 -c Packages > Packages.gz
apt-ftparchive \
    -o APT::FTPArchive::Release::Origin=4agent \
    -o APT::FTPArchive::Release::Label=4agent \
    -o APT::FTPArchive::Release::Suite=stable \
    -o APT::FTPArchive::Release::Codename=stable \
    -o APT::FTPArchive::Release::Architectures="$(printf '%s\n' "${architectures[@]}" | sort -u | tr '\n' ' ')" \
    -o APT::FTPArchive::Release::Description="4agent packages for Debian and Ubuntu" \
    release . > Release.new
mv Release.new Release
touch .nojekyll
cat > index.html <<'HTML'
<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>4agent packages</title>
<style>body{max-width:850px;margin:64px auto;padding:0 24px;background:#10151c;color:#e2e8f0;font:17px/1.7 system-ui}h1{color:#67e8f9}pre{padding:22px;background:#1a2430;border-radius:12px;overflow:auto}a{color:#67e8f9}code{font-size:15px}</style>
<h1>4agent</h1><p>Temporary Linux accounts for AI agents. A terminal UI with SSH keys, optional root access, expiration, and command history.</p>
<h2>Install on Ubuntu / Debian (amd64)</h2>
<pre><code>echo "deb [arch=amd64 trusted=yes] https://hdmain.github.io/4agent ./" \
  | sudo tee /etc/apt/sources.list.d/4agent.list
sudo apt update
sudo apt install 4agent
sudo 4agent</code></pre>
<p>Requires systemd. The package enables the expiration timer. Credentials are stored by default in <code>/var/lib/4agent/exports</code>.</p>
<p><a href="https://github.com/hdmain/4agent">Source and documentation</a> · <a href="Packages">Package index</a></p>
</html>
HTML
