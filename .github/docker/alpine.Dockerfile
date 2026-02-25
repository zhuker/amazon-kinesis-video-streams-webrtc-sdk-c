FROM alpine:3.15.4

RUN apk update && apk upgrade && apk add --no-cache \
    alpine-sdk \
    cmake \
    clang \
    linux-headers \
    perl \
    bash \
    openssl-dev \
    zlib-dev \
    curl-dev
