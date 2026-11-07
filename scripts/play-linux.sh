#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
player=${HITZ_LINUX_PLAYER:-"$root/native/build-linux/nhl_hitz_pro_recomp"}
game_files=${1:-"$root/game_files"}

if [[ ! -x $player || ! -f $game_files/default.xbe ]]; then
    printf 'Build the Linux executable and supply an extracted game directory containing default.xbe.\n' >&2
    exit 1
fi

export RECOMP_GL_VERTEX_UPLOAD=${RECOMP_GL_VERTEX_UPLOAD:-stream}
export RECOMP_GL_VERTEX_INDEXED=${RECOMP_GL_VERTEX_INDEXED:-1}
export RECOMP_GL_TEXTURE_WATCH=${RECOMP_GL_TEXTURE_WATCH:-1}
export RECOMP_GL_TEXTURE_WATCH_FAST=${RECOMP_GL_TEXTURE_WATCH_FAST:-1}
export RECOMP_GL_TEXTURE_WATCH_CLEAR_EVERY=${RECOMP_GL_TEXTURE_WATCH_CLEAR_EVERY:-8}
export RECOMP_GL_TEXTURE_INDEX=${RECOMP_GL_TEXTURE_INDEX:-1}
export RECOMP_PB_IDLE_WAIT_US=${RECOMP_PB_IDLE_WAIT_US:-100}
if [[ -z ${RECOMP_TEXPACK:-} && -d $root/texture_pack ]]; then
    export RECOMP_TEXPACK="$root/texture_pack"
fi

exec "$player" "$game_files"
