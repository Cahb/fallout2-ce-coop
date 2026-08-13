# syntax=docker/dockerfile:1

ARG ALPINE_VERSION=3.22

FROM alpine:${ALPINE_VERSION} AS builder

RUN apk add --no-cache \
        build-base \
        cmake \
        git \
        linux-headers \
        ninja \
        sdl2-dev \
        zlib-dev \
        zlib-static

WORKDIR /src
COPY . .

# f2_server does not use SDL at runtime. Building it as a static musl executable
# lets the final image contain the server and nothing else.
RUN cmake -S . -B /build -G Ninja \
        -DCMAKE_BUILD_TYPE=MinSizeRel \
        -DCMAKE_EXE_LINKER_FLAGS=-static \
        -DFALLOUT_VENDORED=OFF \
        -DZLIB_LIBRARY=/usr/lib/libz.a \
    && cmake --build /build --target f2_server --parallel \
    && strip /build/f2_server

FROM scratch AS fo2-dedicated-server

LABEL org.opencontainers.image.title="Fallout 2 CE dedicated server" \
      org.opencontainers.image.description="Headless Fallout 2 CE co-op dedicated server"

COPY --from=builder --chmod=0555 /build/f2_server /f2_server

WORKDIR /game
USER 1000:1000

EXPOSE 9200/tcp 9201/tcp

ENTRYPOINT ["/f2_server"]
