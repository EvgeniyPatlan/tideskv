# perf sidecar image for profile.sh. It is built FROM the profiled server image (BASE) so that perf, running in the
# server's pid namespace, finds the same binaries at the same paths and resolves TidesKV and TidesDB symbols.
ARG BASE
FROM ${BASE}
RUN apt-get update && apt-get install -y --no-install-recommends linux-tools-generic && rm -rf /var/lib/apt/lists/* \
    && ln -s "$(ls -d /usr/lib/linux-tools/*/ | head -1)perf" /usr/local/bin/perf
ENTRYPOINT []
