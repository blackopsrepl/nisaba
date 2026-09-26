#!/bin/sh
# Install the nisaba agent skill (skills/nisaba/) into a coding harness's
# skills directory.
#
#   skills/install.sh [DEST]
#
# With DEST, copies into DEST/nisaba (creating DEST if needed). Without one,
# installs into every harness skills directory detected on this machine.
#
# Known homes for SKILL.md-style skills:
#   Claude Code   .claude/skills  or  ~/.claude/skills
#   opencode      .opencode/skills  or  ~/.config/opencode/skills
set -eu

here="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
src="$here/nisaba"

if [ ! -f "$src/SKILL.md" ]; then
    echo "install.sh: cannot find SKILL.md next to this script (looked in $src)" >&2
    exit 1
fi

usage() {
    echo "usage: $0 [DEST]" >&2
    echo "  DEST  a skills directory; its nisaba/ subdir is replaced" >&2
    echo "  without DEST: every detected harness skills dir is used" >&2
    exit 2
}

install_into() {
    dest="$1/nisaba"
    rm -rf "$dest"
    mkdir -p "$(dirname -- "$dest")"
    cp -R "$src" "$dest"
    echo "installed: $dest"
}

if [ $# -gt 1 ]; then
    usage
fi

if [ $# -eq 1 ]; then
    case "$1" in
        -h|--help) usage ;;
    esac
    install_into "$1"
    exit 0
fi

found=0
for dir in \
    "$PWD/.claude/skills" \
    "$PWD/.opencode/skills" \
    "$HOME/.claude/skills" \
    "$HOME/.config/opencode/skills"
do
    if [ -d "$dir" ]; then
        install_into "$dir"
        found=1
    fi
done

if [ "$found" -eq 0 ]; then
    echo "install.sh: no existing skills directory found; pass one explicitly:" >&2
    echo "  $0 ~/.claude/skills            # Claude Code (user)" >&2
    echo "  $0 .claude/skills              # Claude Code (project)" >&2
    echo "  $0 ~/.config/opencode/skills   # opencode (user)" >&2
    echo "  $0 .opencode/skills            # opencode (project)" >&2
    exit 1
fi
