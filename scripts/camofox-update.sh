#!/usr/bin/env bash
# Camofox host updater: one command on the box running camofox-browser
# (docker, Makefile targets build/up/down). Replaces the periodic
#   make down && git pull && make build && make up
# dance with build-then-roll: the NEW image builds before the running
# container is touched, so a failed pull or build leaves the old service up,
# and downtime is the seconds of down+up.
#
# Deployment: this script's canonical copy lives in the nyaxus harness repo
# (scripts/camofox-update.sh) - versioned with the integration it supports.
# The daemon is deployed at /agent; Camofox is a separate service you
# deploy wherever you like. Keep the camofox checkout OUT of the deployment
# dir (the agent release process may clobber /agent wholesale, and
# camofox has an independent upstream lifecycle). Copy this script once to
# run beside camofox and re-copy when the repo copy changes. The daemon's
# contract with Camofox is the REST endpoint (:9377) - the daemon never
# reads the camofox source tree, so nesting it would buy nothing.
#
# Usage (on the camofox host, e.g. the same box that runs the daemon):
#   bash camofox-update.sh /path/to/camofox-browser
#   CAMOFOX_DIR=/path/to/camofox-browser bash camofox-update.sh
set -eu

CAMOFOX_DIR="${CAMOFOX_DIR:-${1:-}}"
if [ -z "$CAMOFOX_DIR" ]; then
    echo "usage: CAMOFOX_DIR=/path/to/camofox-browser $0 (or pass the dir as $1)" >&2
    exit 1
fi
cd "$CAMOFOX_DIR" || { echo "camofox dir not found: $CAMOFOX_DIR" >&2; exit 1; }

echo "== camofox update in $CAMOFOX_DIR =="

# Use the configured URL base when rebuilding the image tag.
if [ -f "Makefile" ]; then
    echo "== git pull =="
    if ! git pull --ff-only; then
        echo "git pull failed; keeping the running container, aborting" >&2
        exit 1
    fi
fi

echo "== docker build (cached layers only - no forced rebuild) =="
if ! docker build --build-arg ARCH="$(uname -m)" \
    --build-arg CAMOFOX_VERSION=135.0.1 \
    --build-arg CAMOFOX_RELEASE=beta.24 \
    -t camofox-browser:135.0.1-x86_64 . ; then
    echo "docker build failed; keeping the running container" >&2
    exit 1
fi

echo "== roll the container =="
was_running=0
if [ "$(docker inspect -f '{{.State.Running}}' camofox-browser 2>/dev/null)" = "true" ]; then
    was_running=1
fi
if [ "$was_running" = "1" ]; then
    docker stop camofox-browser > /dev/null
    docker rm camofox-browser > /dev/null
fi
docker run -d --restart unless-stopped --name camofox-browser \
    --shm-size=2g -p 9377:9377 camofox-browser:135.0.1-x86_64 > /dev/null

echo "== health poll =="
up=0
for _ in $(seq 1 30); do
    if curl -sf "http://127.0.0.1:9377/health" > /dev/null 2>&1; then
        up=1
        break
    fi
    sleep 1
done
if [ "$up" = "1" ]; then
    echo "camofox updated and healthy"
else
    echo "warning: container is up but /health did not answer in 30s" >&2
fi
