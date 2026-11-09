# syntax=docker/dockerfile:1
# MiniVectorDB — multi-stage build.
#
# Stage 1 compiles the database from source with only build tooling present;
# stage 2 carries nothing but the binary and the C++ runtime it needs.  The
# final image runs the program in the foreground, reading SQL from stdin and
# writing results to stdout, exactly as section 3.1 of the task statement
# requires.
FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential \
      cmake \
      ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build --parallel "$(nproc)" \
 && strip build/minivectordb

# Sanity check at build time: the binary must start, answer one statement and
# exit cleanly.  A broken image then fails the build instead of the evaluation.
RUN printf 'CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(2));\n' \
    | ./build/minivectordb --data-dir /tmp/smoke > /tmp/smoke.out \
 && grep -q 'OK' /tmp/smoke.out

FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends libstdc++6 \
 && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/minivectordb /usr/local/bin/minivectordb

# All persistent state lives under /data (task statement 2.1 / 3.1).
RUN mkdir -p /data
VOLUME ["/data"]

# Foreground process, no ports, no daemon: the driver attaches to stdin/stdout.
ENTRYPOINT ["/usr/local/bin/minivectordb", "--data-dir", "/data"]
