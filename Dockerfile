FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ make && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY Makefile .
COPY src/ src/
COPY profiles/ profiles/
RUN mkdir /artifacts && \
    case "$(uname -m)" in x86_64) arch=-march=x86-64-v2 ;; *) arch='' ;; esac && \
    make ARCH="$arch" && mv pipeline /artifacts/ && \
    if [ "$(uname -m)" = x86_64 ]; then \
        make ARCH='-march=skylake-avx512 -mtune=cascadelake' \
            CXXFLAGS='-O3 -DNDEBUG -fprofile-prefix-path=/build -fprofile-use=profiles -Werror=missing-profile -Werror=coverage-mismatch' && \
        mv pipeline /artifacts/pipeline-avx512; \
    fi

FROM debian:bookworm-slim
COPY --from=build /artifacts/ /usr/local/bin/
COPY --chmod=0755 scripts/entrypoint.sh /usr/local/bin/run-pipeline
USER 1000:1000
ENTRYPOINT ["/usr/local/bin/run-pipeline"]
