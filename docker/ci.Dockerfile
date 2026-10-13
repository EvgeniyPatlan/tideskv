# Toolchain image mirroring the GitHub Actions ubuntu-24.04 jobs in .github/workflows/ci.yml.
# Used by scripts/docker-ci.sh.
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates git cmake ninja-build build-essential python3 curl util-linux \
      clang-18 llvm-18 libclang-rt-18-dev clang-tidy-18 clang-format-18 \
      liblz4-dev libzstd-dev \
    && rm -rf /var/lib/apt/lists/*
ARG GO_VERSION=1.22.12
RUN curl -fsSL "https://go.dev/dl/go${GO_VERSION}.linux-amd64.tar.gz" | tar -C /usr/local -xz
ENV PATH=/usr/local/go/bin:$PATH
