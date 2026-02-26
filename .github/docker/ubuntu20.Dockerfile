FROM public.ecr.aws/ubuntu/ubuntu:20.04_stable

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y software-properties-common cmake git gdb pkg-config build-essential && \
    add-apt-repository -y ppa:ubuntu-toolchain-r/test && \
    add-apt-repository 'deb http://archive.ubuntu.com/ubuntu/ trusty main' && \
    add-apt-repository 'deb http://archive.ubuntu.com/ubuntu/ trusty universe' && \
    apt-get -q update && \
    apt-get -y install gcc-4.4 && \
    rm -rf /var/lib/apt/lists/*
