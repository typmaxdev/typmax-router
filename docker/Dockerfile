# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The typmax-router authors
# Linux x86_64 build + test image (the deployment target is linux/amd64).
#   docker build --platform linux/amd64 -f docker/Dockerfile -t typmax-router:linux .
# The image builds the binary and runs the synthetic and determinism suites;
# a red test fails the build.
# The `image` workflow (.github/workflows/image.yml) builds this same file and publishes
# its runtime stage as ghcr.io/typmaxdev/typmax-router (MAINTAINING.md "Container image").
FROM debian:bookworm-slim AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends clang cmake ninja-build patch python3 \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt PROTOCOL.md REUSE.toml ./
COPY LICENSES LICENSES
COPY vendor vendor
COPY patches patches
COPY shim shim
COPY src src
COPY tests tests
COPY tools tools
RUN cmake -S . -B /build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release \
 && ninja -C /build -j4 \
 && strip /build/typmax-router
# GOLDEN=0 (the release watcher, before a person re-records the golden lines)
# checks determinism within and across processes but not against the committed
# lines, and records this build's lines in /build/golden.jsonl instead, for the
# watcher to compare with its other builds (docker create + docker cp).
ARG GOLDEN=1
RUN python3 tests/run_tests.py --binary /build/typmax-router --suite synthetic \
 && if [ "$GOLDEN" = 1 ]; then python3 tests/run_tests.py --binary /build/typmax-router --suite determinism; \
    else python3 tests/run_tests.py --binary /build/typmax-router --only t_determinism \
      && python3 tests/run_tests.py --binary /build/typmax-router --record-golden /build/golden.jsonl; fi

# The runtime image: the binary alone (it links only libstdc++/libc).
FROM debian:bookworm-slim
COPY --from=build /build/typmax-router /usr/local/bin/typmax-router
USER 65534:65534
ENTRYPOINT ["/usr/local/bin/typmax-router"]
