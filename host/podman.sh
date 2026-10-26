#!/bin/sh
# OpenX6X00 - the GUI on a PC without Linux (macOS, Windows): host/build.sh in an Ubuntu
# container under podman, the SDL window in a virtual X screen of 800x480 (PulseAudio without a
# sound card). The screen over VNC, keys as in host/README.md: open vnc://localhost:5901, password x6100
#
#   host/podman.sh                 build and run
#   host/podman.sh shot <theme>... choose each theme through the config socket, save shots/<theme>.png
#   host/podman.sh theme <theme>   switch the theme
#   host/podman.sh shell           a shell in the running container
#
# build-host/ lives in the podman volume x6100-build, not on the Mac's disk.
set -e
cd "$(dirname "$0")/.."
IMG=x6100-host
CT=x6100-gui

image() {
    podman image exists $IMG && return
    podman build -t $IMG -f - . <<'EOF'
FROM ubuntu:24.04
RUN apt-get update -q && DEBIAN_FRONTEND=noninteractive apt-get install -yq --no-install-recommends \
    build-essential cmake ninja-build git curl ca-certificates bzip2 pkg-config sqlite3 libsqlite3-dev \
    libsdl2-dev libdrm-dev libxkbcommon-dev libnm-dev libdbus-glib-1-dev libpng-dev libjpeg-dev \
    libsndfile1-dev libfftw3-dev libudev-dev libpulse-dev libasound2-dev libgps-dev \
    autoconf automake libtool xvfb x11vnc pulseaudio imagemagick socat && rm -rf /var/lib/apt/lists/*
EOF
}

case "${1:-run}" in
run)
    image
    podman rm -f $CT >/dev/null 2>&1 || true
    podman run -d --name $CT -p 127.0.0.1:5901:5900 -v "$PWD":/src -v x6100-build:/src/build-host -w /src $IMG \
        sh -c 'host/build.sh build && mkdir -p build-host/data/themes && cp themes/*.json build-host/data/themes/ &&
               { Xvfb :1 -screen 0 800x480x24 -nolisten tcp & } && pulseaudio -D --exit-idle-time=-1 &&
               sleep 2 && { x11vnc -display :1 -forever -shared -passwd x6100 -q >/dev/null 2>&1 & } && DISPLAY=:1 exec build-host/src/x6100_gui'
    echo "building, then: open vnc://localhost:5901 (password x6100)   (podman logs -f $CT)"
    ;;
shot)
    shift; mkdir -p shots
    for t in "$@"; do
        podman exec $CT sh -c "echo '{\"cmd\":\"disp_set\",\"text\":\"theme\",\"theme\":\"$t\"}' | socat - UNIX-CONNECT:/tmp/x6100_cfg.sock
            sleep 4; DISPLAY=:1 import -window root /tmp/shot.png"
        podman cp $CT:/tmp/shot.png "shots/$(echo "$t" | tr ' ' _).png"
        echo "shots/$(echo "$t" | tr ' ' _).png"
    done
    ;;
theme)
    podman exec $CT sh -c "echo '{\"cmd\":\"disp_set\",\"text\":\"theme\",\"theme\":\"$2\"}' | socat - UNIX-CONNECT:/tmp/x6100_cfg.sock" ;;
shell) exec podman exec -it $CT bash ;;
*) echo "usage: host/podman.sh [run|shot <theme>...|theme <theme>|shell]" >&2; exit 2 ;;
esac
