FROM docker.io/library/debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates curl xz-utils gcc libc6-dev cmake ninja-build git \
    libxinerama-dev libxcursor-dev libxrandr-dev libxi-dev libxext-dev libx11-dev libgl-dev \
    && rm -rf /var/lib/apt/lists/*
RUN curl -fsSL https://ziglang.org/download/0.16.0/zig-x86_64-linux-0.16.0.tar.xz -o /tmp/zig.tar.xz \
    && echo '70e49664a74374b48b51e6f3fdfbf437f6395d42509050588bd49abe52ba3d00  /tmp/zig.tar.xz' | sha256sum -c - \
    && mkdir /opt/zig && tar -xJf /tmp/zig.tar.xz -C /opt/zig --strip-components=1 \
    && ln -s /opt/zig/zig /usr/local/bin/zig && rm /tmp/zig.tar.xz

WORKDIR /src
COPY CMakeLists.txt bin2header.cmake main.c ./
COPY fonts/ fonts/
RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
