FROM debian:stretch
LABEL maintainer="Memeium Developers <memeiumcore@gmail.com>"
LABEL description="Dockerised MemeiumCore, built from Travis"

RUN apt-get update && apt-get -y upgrade && apt-get clean && rm -fr /var/cache/apt/*

COPY bin/* /usr/bin/
