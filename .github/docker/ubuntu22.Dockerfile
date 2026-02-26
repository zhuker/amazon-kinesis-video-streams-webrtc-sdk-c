FROM public.ecr.aws/ubuntu/ubuntu:22.04_stable

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    clang \
    clang-format \
    git \
    libcurl4-openssl-dev \
    pkg-config \
    file \
    gcc-mips-linux-gnu \
    g++-mips-linux-gnu \
    gcc-arm-linux-gnueabihf \
    g++-arm-linux-gnueabihf \
    gcc-aarch64-linux-gnu \
    g++-aarch64-linux-gnu \
    && rm -rf /var/lib/apt/lists/*
