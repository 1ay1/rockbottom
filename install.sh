#!/bin/sh
# Install the latest rockbottom release without a compiler or package manager.
# Usage from any interactive shell (bash, zsh, fish, etc.):
# curl -fsSL https://raw.githubusercontent.com/1ay1/rockbottom/master/install.sh | sh
# The installer itself is deliberately POSIX sh, so this works independently
# of the shell the user has configured.
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

# Quote a literal path for a shell startup file without evaluating it now.
shell_quote() {
    printf "'"
    printf '%s' "$1" | sed "s/'/'\\\\''/g"
    printf "'"
}

add_posix_path() {
    file=$1
    bin_dir=$2
    marker="# rockbottom installer: add $bin_dir to PATH"
    if [ -f "$file" ] && grep -F "$marker" "$file" >/dev/null 2>&1; then
        return
    fi
    mkdir -p "$(dirname "$file")"
    {
        printf '\n%s\n' "$marker"
        printf 'export PATH=%s:"$PATH"\n' "$(shell_quote "$bin_dir")"
    } >> "$file"
}

add_fish_path() {
    file=$1
    bin_dir=$2
    marker="# rockbottom installer: add $bin_dir to PATH"
    if [ -f "$file" ] && grep -F "$marker" "$file" >/dev/null 2>&1; then
        return
    fi
    mkdir -p "$(dirname "$file")"
    {
        printf '\n%s\n' "$marker"
        printf 'set -gx PATH %s $PATH\n' "$(shell_quote "$bin_dir")"
    } >> "$file"
}

configure_path() {
    bin_dir=$1
    # `sh` cannot modify its parent shell's environment. Persist the entry in
    # the startup files used by supported interactive shells instead.
    add_posix_path "$HOME/.profile" "$bin_dir"
    add_posix_path "$HOME/.bashrc" "$bin_dir"
    add_posix_path "$HOME/.zshrc" "$bin_dir"
    add_fish_path "$HOME/.config/fish/config.fish" "$bin_dir"
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
configure_path "$prefix/bin"
echo "Installed rb to $prefix/bin/rb"
"$prefix/bin/rb" --version
case ":${PATH}:" in
    *":$prefix/bin:"*) echo "rb is ready: run rb" ;;
    *) echo "Added $prefix/bin to PATH for future Bash, zsh, fish, and login sh sessions. Open a new terminal, then run: rb" ;;
esac
