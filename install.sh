#!/bin/sh
# Install the latest mano release for Linux or macOS.
#   curl -fsSL https://github.com/phdgamer0/mano/releases/latest/download/install.sh | sh
# Env: PREFIX (default ~/.local), MANO_VERSION (default: latest release).
set -eu

REPO=phdgamer0/mano
PREFIX=${PREFIX:-$HOME/.local}

case "$(uname -s)" in
    Linux) os=linux ;;
    Darwin) os=macos ;;
    *) echo "mano: unsupported OS $(uname -s); on Windows use install.ps1" >&2; exit 1 ;;
esac
case "$(uname -m)" in
    x86_64 | amd64) arch=x86_64 ;;
    aarch64 | arm64) arch=$([ "$os" = macos ] && echo arm64 || echo aarch64) ;;
    *) echo "mano: no prebuilt binary for $(uname -m); build from source" >&2; exit 1 ;;
esac
if [ "$os-$arch" = macos-x86_64 ]; then
    echo "mano: no prebuilt binary for Intel macOS; build from source" >&2
    exit 1
fi

tag=${MANO_VERSION:-$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" |
    sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p')}
[ -n "$tag" ] || { echo "mano: cannot determine latest release" >&2; exit 1; }
case "$tag" in v*) ;; *) tag="v$tag" ;; esac
name="mano-${tag#v}-$os-$arch"
base="https://github.com/$REPO/releases/download/$tag"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM
echo "downloading $name.tar.gz"
curl -fsSL -o "$tmp/$name.tar.gz" "$base/$name.tar.gz"
curl -fsSL -o "$tmp/SHA256SUMS" "$base/SHA256SUMS"

want=$(grep " $name.tar.gz\$" "$tmp/SHA256SUMS" | cut -d' ' -f1)
if command -v sha256sum >/dev/null 2>&1; then
    got=$(sha256sum "$tmp/$name.tar.gz" | cut -d' ' -f1)
else
    got=$(shasum -a 256 "$tmp/$name.tar.gz" | cut -d' ' -f1)
fi
[ -n "$want" ] && [ "$want" = "$got" ] || { echo "mano: checksum mismatch" >&2; exit 1; }

tar -xzf "$tmp/$name.tar.gz" -C "$tmp"
mkdir -p "$PREFIX/bin"
install -m 755 "$tmp/$name/mano" "$PREFIX/bin/mano"
echo "installed $PREFIX/bin/mano ($tag)"
case ":$PATH:" in *":$PREFIX/bin:"*) ;; *) echo "note: add $PREFIX/bin to your PATH" ;; esac
