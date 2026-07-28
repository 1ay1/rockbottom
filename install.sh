#!/bin/sh
# Install the latest rockbottom release without a compiler or package manager.
# Usage: curl -fsSL https://raw.githubusercontent.com/1ay1/rockbottom/main/install.sh | sh
set -eu

repo="1ay1/rockbottom"
prefix="${HOME}/.local"
version="latest"

usage() {
    cat <<'EOF'
Usage: install.sh [--prefix DIR] [--version VERSION]

Installs rb into DIR/bin (default: ~/.local/bin).
The Linux release binary is fully static: it has no shared-library or glibc
requirement. macOS uses only libraries supplied by macOS itself.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --prefix)  prefix=${2:?--prefix needs a directory}; shift 2 ;;
        --version) version=${2:?--version needs a version}; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "error: unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

os=$(uname -s)
arch=$(uname -m)
case "$os:$arch" in
    Linux:x86_64|Linux:amd64) asset="rb-linux-x86_64" ;;
    Linux:aarch64|Linux:arm64) asset="rb-linux-arm64" ;;
    Darwin:arm64)              asset="rb-macos-arm64" ;;
    *)
        echo "error: no prebuilt rb release for $os/$arch" >&2
        echo "See https://github.com/$repo for source builds." >&2
        exit 1
        ;;
esac

if command -v curl >/dev/null 2>&1; then
    fetch() { curl -fL --retry 3 --retry-delay 1 -o "$2" "$1"; }
elif command -v wget >/dev/null 2>&1; then
    fetch() { wget -O "$2" "$1"; }
else
    echo "error: curl or wget is required to download rb" >&2
    exit 1
fi

case "$version" in
    latest) base="https://github.com/$repo/releases/latest/download" ;;
    v*)    base="https://github.com/$repo/releases/download/$version" ;;
    *)     base="https://github.com/$repo/releases/download/v$version" ;;
esac

tmp=${TMPDIR:-/tmp}/rb-install-$$
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp"

echo "Downloading $asset..."
fetch "$base/$asset" "$tmp/$asset"
fetch "$base/SHA256SUMS" "$tmp/SHA256SUMS"

expected=$(awk -v name="$asset" '$2 == name { print $1; exit }' "$tmp/SHA256SUMS")
if [ -z "$expected" ]; then
    echo "error: checksum for $asset is missing from the release" >&2
    exit 1
fi
if command -v sha256sum >/dev/null 2>&1; then
    actual=$(sha256sum "$tmp/$asset" | awk '{print $1}')
elif command -v shasum >/dev/null 2>&1; then
    actual=$(shasum -a 256 "$tmp/$asset" | awk '{print $1}')
else
    echo "error: sha256sum or shasum is required to verify rb" >&2
    exit 1
fi
if [ "$actual" != "$expected" ]; then
    echo "error: checksum verification failed; refusing to install" >&2
    exit 1
fi

mkdir -p "$prefix/bin"
install -m 0755 "$tmp/$asset" "$prefix/bin/rb"
echo "Installed rb to $prefix/bin/rb"
"$prefix/bin/rb" --version
case ":${PATH}:" in
    *":$prefix/bin:"*) ;;
    *) echo "Add $prefix/bin to PATH, then run: rb" ;;
esac
