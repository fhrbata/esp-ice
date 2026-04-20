#!/bin/sh
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# esp-ice installer for Linux and macOS.
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/fhrbata/esp-ice/main/install.sh | sh
#
# Environment overrides:
#   ICE_VERSION       Release tag (default: latest). Accepts "v0.2.0" or "0.2.0".
#   ICE_INSTALL_DIR   Destination directory (default: $HOME/.local/bin).
#   ICE_ARCH          Override detected architecture (e.g. for armel vs armhf).
#   ICE_REPO          GitHub repository (default: fhrbata/esp-ice).

set -eu

: "${ICE_REPO:=fhrbata/esp-ice}"
: "${ICE_VERSION:=latest}"
: "${ICE_INSTALL_DIR:=$HOME/.local/bin}"

die () {
	echo "error: $*" >&2
	exit 1
}

need () {
	command -v "$1" >/dev/null 2>&1 || die "'$1' is required but not installed"
}

need curl
need tar

case "$(uname -s)" in
	Linux)  os=linux ;;
	Darwin) os=macos ;;
	*)
		die "unsupported OS $(uname -s) (Linux/macOS only; use install.ps1 on Windows)"
		;;
esac

arch="$(uname -m)"
case "$arch" in
	x86_64|amd64)   arch=amd64 ;;
	aarch64|arm64)  arch=arm64 ;;
	armv7*|armv6*)  arch=armhf ;;
	i?86)           arch=i386 ;;
	ppc64le)        arch=ppc64el ;;
	riscv64)        arch=riscv64 ;;
	s390x)          arch=s390x ;;
	*)              die "unsupported architecture $arch" ;;
esac

[ -n "${ICE_ARCH:-}" ] && arch="$ICE_ARCH"

if [ "$os" = macos ]; then
	case "$arch" in
		amd64|arm64) ;;
		*) die "macOS only supports amd64 and arm64 (got $arch)" ;;
	esac
fi

if [ "$ICE_VERSION" = latest ]; then
	echo "Resolving latest version..."
	ver=$(curl -fsSL "https://api.github.com/repos/$ICE_REPO/releases/latest" \
		| grep -o '"tag_name": *"v[^"]*"' \
		| head -n1 \
		| sed 's/.*"v\([^"]*\)".*/\1/')
	[ -n "$ver" ] || die "failed to resolve latest release from $ICE_REPO"
else
	ver="${ICE_VERSION#v}"
fi

pkg="ice-$ver-$os-$arch.tar.gz"
url="https://github.com/$ICE_REPO/releases/download/v$ver/$pkg"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

echo "Downloading $pkg..."
curl -fsSL -o "$tmp/$pkg" "$url" || die "download failed: $url"

echo "Extracting..."
tar -xzf "$tmp/$pkg" -C "$tmp"

mkdir -p "$ICE_INSTALL_DIR"
cp "$tmp/ice-$ver/bin/ice" "$ICE_INSTALL_DIR/ice"
chmod 755 "$ICE_INSTALL_DIR/ice"

cat <<EOF

  ice $ver installed
  ───────────────────────────────────────────────
  path:    $ICE_INSTALL_DIR/ice
  version: $ver
  target:  $os-$arch

Next steps:
EOF

case ":$PATH:" in
	*":$ICE_INSTALL_DIR:"*) ;;
	*)
		cat <<EOF

  # Add to PATH ($ICE_INSTALL_DIR is not currently in \$PATH)
  export PATH="$ICE_INSTALL_DIR:\$PATH"
EOF
		;;
esac

cat <<'EOF'

  # Get started -- see the GETTING STARTED section for tab completion
  # setup and the rest of the onboarding walk-through.
  ice
EOF
echo
