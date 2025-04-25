#!/usr/bin/env bash

export LC_ALL=C

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd $DIR/.. || exit

DOCKER_IMAGE=${DOCKER_IMAGE:-The-Memeium-Endeavor/memeiumd-develop}
DOCKER_TAG=${DOCKER_TAG:-latest}

BUILD_DIR=${BUILD_DIR:-.}

rm docker/bin/*
mkdir docker/bin
cp $BUILD_DIR/src/memeiumd docker/bin/
cp $BUILD_DIR/src/memeium-cli docker/bin/
cp $BUILD_DIR/src/memeium-tx docker/bin/
strip docker/bin/memeiumd
strip docker/bin/memeium-cli
strip docker/bin/memeium-tx

docker build --pull -t $DOCKER_IMAGE:$DOCKER_TAG -f docker/Dockerfile docker
