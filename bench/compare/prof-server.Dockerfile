# Frame-pointer TidesKV server image for profile.sh: tideskv.Dockerfile with -fno-omit-frame-pointer, so that
# perf --call-graph fp can walk the stacks. Built from the committed tree only; profile.sh injects this file at the root.
FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates git cmake ninja-build build-essential python3 liblz4-dev libzstd-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
ARG TIDESKV_ALLOCATOR=mimalloc
RUN cmake --preset release -DCMAKE_C_FLAGS=-fno-omit-frame-pointer -DCMAKE_CXX_FLAGS=-fno-omit-frame-pointer \
      -DTIDESKV_ALLOCATOR="$TIDESKV_ALLOCATOR" -DTIDESKV_BUILD_TESTS=OFF -DTIDESKV_BUILD_BENCH=OFF \
    && cmake --build --preset release --target tideskv-server -j"$(nproc)"

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends liblz4-1 libzstd1 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/release/src/tideskv-server /usr/local/bin/tideskv-server
COPY --from=build /src/build/release/_deps/tidesdb-build/libtidesdb.so* /usr/local/lib/
RUN ldconfig
VOLUME /data
ENTRYPOINT ["/usr/local/bin/tideskv-server"]
