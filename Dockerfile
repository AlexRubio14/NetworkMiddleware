# ─── Stage 1: Builder ────────────────────────────────────────────────────────
# Ubuntu 24.04 used for both build and runtime to guarantee glibc/SFML ABI
# compatibility (Alpine uses musl libc which conflicts with SFML).
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y \
        build-essential \
        cmake \
        libsfml-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Build both executables in Release mode (no debug symbols = smaller, faster).
RUN cmake -B /build \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTS=OFF \
    && cmake --build /build --parallel

# ─── Stage 2: NetServer runtime ──────────────────────────────────────────────
FROM ubuntu:24.04 AS server

# Only the SFML network + system shared libraries are needed at runtime.
RUN apt-get update && apt-get install -y \
        libsfml-network2.6 \
        libsfml-system2.6 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /build/Server/NetServer /usr/local/bin/NetServer

EXPOSE 7777/udp

CMD ["NetServer"]

# ─── Stage 3: HeadlessBot runtime ────────────────────────────────────────────
FROM ubuntu:24.04 AS bot

RUN apt-get update && apt-get install -y \
        libsfml-network2.6 \
        libsfml-system2.6 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /build/HeadlessBot/HeadlessBot /usr/local/bin/HeadlessBot

# Defaults for network_mode: host (Linux).
# Override SERVER_HOST when using bridge networking on Docker Desktop / Windows.
ENV SERVER_HOST=127.0.0.1
ENV SERVER_PORT=7777

CMD ["HeadlessBot"]
