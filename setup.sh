#!/bin/sh
# issuewatch setup: build packages, the Ollama server, and -- optionally -- the
# model weights. Safe to re-run: every step checks before it acts.
#
#   ./setup.sh             install what is missing
#   ./setup.sh --check     report only; installs nothing, pulls nothing
#
# Which weights, chosen by WEIGHTS (a make variable too: `make setup WEIGHTS=small`):
#
#   auto     (default) exactly what src/config.h's JUDGE_MODE needs:
#            JUDGE_API needs none, JUDGE_LOCAL needs OLLAMA_MODEL,
#            JUDGE_HYBRID adds OLLAMA_SCREEN_MODEL
#   none     skip weights entirely -- Ollama too, if you judge through the API
#   small    OLLAMA_MODEL_SMALL only, for a card that cannot hold the default;
#            run with `--model` naming it, because the binary still defaults
#            to OLLAMA_MODEL
#   default  OLLAMA_MODEL only
#   all      default and small, plus the screen model under JUDGE_HYBRID
#
# Weights are the one expensive, optional thing here -- gigabytes of download
# that an API-judged install never touches -- which is why they get a switch
# and nothing else does.

set -eu

cd "$(dirname "$0")"

CHECK=0
case "${1:-}" in
    --check) CHECK=1 ;;
    "")      ;;
    -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
    *) echo "usage: $0 [--check]" >&2; exit 2 ;;
esac
WEIGHTS="${WEIGHTS:-auto}"

# Colour only on a terminal: piped into a log, escapes are just noise.
if [ -t 1 ]; then G='\033[32m' Y='\033[33m' R='\033[31m' N='\033[0m'; else G= Y= R= N=; fi
ok()   { printf "  ${G}ok${N}    %s\n" "$*"; }
miss() { printf "  ${Y}need${N}  %s\n" "$*"; }
note() { printf '        %s\n' "$*"; }
die()  { printf "${R}error:${N} %s\n" "$*" >&2; exit 1; }
MISSING=0

# ---------------------------------------------------------------- config.h
#
# Resolved by the preprocessor rather than grepped, so an expression, a
# nested macro or a future #if still yields the value the binary compiles.
cfg() {
    printf '#include "config.h"\n@@%s@@\n' "$1" |
        cc -E -P -Isrc -x c - 2>/dev/null |
        sed -n 's/^@@\(.*\)@@$/\1/p' | tr -d '" '
}

# ---------------------------------------------------------------- packages
echo "build requirements"

need_pkgs=""
command -v cc         >/dev/null 2>&1 || need_pkgs="$need_pkgs compiler"
command -v make       >/dev/null 2>&1 || need_pkgs="$need_pkgs make"
command -v pkg-config >/dev/null 2>&1 || need_pkgs="$need_pkgs pkg-config"
# 7.62 is the floor for curl_multi_poll(); the whole I/O loop is built on it.
if ! pkg-config --atleast-version=7.62 libcurl 2>/dev/null; then
    need_pkgs="$need_pkgs libcurl-dev"
fi

if [ -z "$need_pkgs" ]; then
    ok "cc, make, pkg-config, libcurl $(pkg-config --modversion libcurl)"
    # Not fatal: HTTP/1.1 works, it just gives up multiplexing the fan-out.
    if command -v curl-config >/dev/null 2>&1 &&
       ! curl-config --features | grep -q HTTP2; then
        note "libcurl is built without HTTP/2; requests will not multiplex"
    fi
else
    miss "missing:$need_pkgs"
    MISSING=1
    if [ -r /etc/os-release ]; then . /etc/os-release; fi
    case " ${ID:-} ${ID_LIKE:-} " in
        *" debian "*|*" ubuntu "*)
            install="sudo apt-get install -y build-essential pkg-config libcurl4-openssl-dev" ;;
        *" fedora "*|*" rhel "*)
            install="sudo dnf install -y gcc make pkgconf-pkg-config libcurl-devel" ;;
        *" arch "*)
            install="sudo pacman -S --needed base-devel pkgconf curl" ;;
        *)
            install="" ;;
    esac
    if [ -z "$install" ]; then
        note "unknown distro: install a C compiler, make, pkg-config and"
        note "libcurl >= 7.62 development headers, then re-run"
        [ "$CHECK" -eq 1 ] || exit 1
    elif [ "$CHECK" -eq 1 ]; then
        note "would run: $install"
    else
        note "running: $install"
        $install
        MISSING=0
    fi
fi

for t in gh git; do
    if command -v "$t" >/dev/null 2>&1; then
        ok "$t (optional)"
    else
        note "$t not found -- optional: gh supplies GH_TOKEN and creates the gist"
    fi
done

# ----------------------------------------------------------------- weights
JUDGE_MODE=$(cfg JUDGE_MODE)
JUDGE_LOCAL=$(cfg JUDGE_LOCAL)
JUDGE_API=$(cfg JUDGE_API)
JUDGE_HYBRID=$(cfg JUDGE_HYBRID)
M_DEFAULT=$(cfg OLLAMA_MODEL)
M_SMALL=$(cfg OLLAMA_MODEL_SMALL)
M_SCREEN=$(cfg OLLAMA_SCREEN_MODEL)

[ -n "$JUDGE_MODE" ] && [ -n "$M_DEFAULT" ] ||
    die "could not read JUDGE_MODE / OLLAMA_MODEL from src/config.h"

case "$JUDGE_MODE" in
    "$JUDGE_LOCAL")  mode_name=JUDGE_LOCAL ;;
    "$JUDGE_API")    mode_name=JUDGE_API ;;
    "$JUDGE_HYBRID") mode_name=JUDGE_HYBRID ;;
    *)               die "JUDGE_MODE $JUDGE_MODE is not one of the three modes" ;;
esac

case "$WEIGHTS" in
    auto)
        case "$mode_name" in
            JUDGE_API)    models="" ;;
            JUDGE_LOCAL)  models="$M_DEFAULT" ;;
            JUDGE_HYBRID) models="$M_DEFAULT $M_SCREEN" ;;
        esac ;;
    none)    models="" ;;
    small)   models="$M_SMALL" ;;
    default) models="$M_DEFAULT" ;;
    all)
        models="$M_DEFAULT $M_SMALL"
        if [ "$mode_name" = JUDGE_HYBRID ]; then models="$models $M_SCREEN"; fi ;;
    *) die "WEIGHTS=$WEIGHTS: want auto, none, small, default or all" ;;
esac

echo
echo "judge: $mode_name, WEIGHTS=$WEIGHTS -> ${models:-no local models}"

if [ "$mode_name" = JUDGE_API ] && [ -n "$models" ]; then
    note "JUDGE_API never calls Ollama; these weights will sit unused"
fi
if [ "$WEIGHTS" = small ]; then
    note "the binary defaults to $M_DEFAULT: run it as ./issuewatch --model $M_SMALL"
fi

if [ -z "$models" ]; then
    [ "$mode_name" = JUDGE_API ] || [ "$WEIGHTS" = none ] ||
        die "no models resolved for $mode_name"
    if [ "$mode_name" != JUDGE_API ]; then
        note "skipping weights by request; the judge fails every batch until a"
        note "model named in src/config.h is pulled"
    fi
    echo
    [ "$MISSING" -eq 0 ] && echo "done." || echo "some requirements are still missing."
    exit "$MISSING"
fi

# ----------------------------------------------------------------- ollama
echo
echo "ollama"

OLLAMA=$(command -v ollama 2>/dev/null || true)
if [ -z "$OLLAMA" ] && [ -x "$HOME/.local/bin/ollama" ]; then
    OLLAMA="$HOME/.local/bin/ollama"     # rootless installs are often not on PATH
fi

if [ -z "$OLLAMA" ]; then
    miss "ollama is not installed"
    note "install it from https://ollama.com/download, then re-run this script."
    note "Not done automatically: the official installer runs as root."
    exit 1
fi
ok "$OLLAMA"

# A rootless install has no service, so the server is whatever someone last
# typed into a terminal -- and a reboot silently ends it. That exact failure
# made a whole cycle judge nothing: every batch came back "Couldn't connect".
# The system installer ships its own unit; only the rootless case needs ours.
system_unit=0
if systemctl list-unit-files ollama.service 2>/dev/null | grep -q '^ollama.service'; then
    system_unit=1
fi

if [ "$system_unit" -eq 1 ]; then
    ok "system ollama.service (from the official installer)"
elif systemctl --user is-enabled ollama.service >/dev/null 2>&1; then
    ok "user ollama.service, enabled"
elif [ "$CHECK" -eq 1 ]; then
    miss "no service: the server will not survive a reboot"
    note "would install systemd/ollama.service as a user unit"
    MISSING=1
else
    mkdir -p "$HOME/.config/systemd/user"
    sed "s|@OLLAMA@|$OLLAMA|" systemd/ollama.service \
        > "$HOME/.config/systemd/user/ollama.service"
    systemctl --user daemon-reload
    systemctl --user enable --now ollama.service
    ok "installed and started user ollama.service"
fi

# ----------------------------------------------------------------- pull
echo
echo "weights"

if ! curl -fsS http://127.0.0.1:11434/api/tags >/dev/null 2>&1; then
    if [ "$CHECK" -eq 1 ]; then
        miss "ollama server is not answering on 127.0.0.1:11434"
        MISSING=1
        for m in $models; do note "would pull $m"; done
        exit "$MISSING"
    fi
    # Just enabled above; give it a moment to bind before pulling.
    i=0
    until curl -fsS http://127.0.0.1:11434/api/tags >/dev/null 2>&1; do
        i=$((i + 1))
        [ "$i" -le 15 ] || die "ollama server did not come up on 127.0.0.1:11434"
        sleep 1
    done
fi

have=$("$OLLAMA" list 2>/dev/null | awk 'NR > 1 { print $1 }')
for m in $models; do
    if printf '%s\n' "$have" | grep -qx "$m"; then
        ok "$m"
    elif [ "$CHECK" -eq 1 ]; then
        miss "$m (not pulled)"
        MISSING=1
    else
        note "pulling $m -- this is the multi-gigabyte step"
        "$OLLAMA" pull "$m"
        ok "$m"
    fi
done

echo
if [ "$MISSING" -eq 0 ]; then
    echo "done. Next: install -Dm600 src/config.example ~/.config/issuewatch/config"
else
    echo "some requirements are still missing (see 'need' above)."
fi
exit "$MISSING"
