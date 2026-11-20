FROM python:3.12-slim-bookworm AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ make && rm -rf /var/lib/apt/lists/*
RUN pip install --no-cache-dir --no-deps pyarrow==21.0.0 && \
    mkdir -p /opt/arrow/lib && D=$(python -c 'import sysconfig; print(sysconfig.get_path("purelib") + "/pyarrow")') && \
    cp -r "$D/include" /opt/arrow/ && cp "$D/libarrow.so.2100" "$D/libparquet.so.2100" /opt/arrow/lib/
WORKDIR /build
COPY Makefile .
COPY src/ src/
RUN mkdir /artifacts && \
    case "$(uname -m)" in x86_64) arch=-march=x86-64-v2 ;; *) arch='' ;; esac && \
    make ARCH="$arch" ARROW_PREFIX=/opt/arrow ARROW_LINK='-l:libparquet.so.2100 -l:libarrow.so.2100' && \
    mv pipeline /artifacts/ && \
    if [ "$(uname -m)" = x86_64 ]; then \
        make ARCH='-march=skylake-avx512 -mtune=cascadelake' ARROW_PREFIX=/opt/arrow ARROW_LINK='-l:libparquet.so.2100 -l:libarrow.so.2100' && \
        mv pipeline /artifacts/pipeline-avx512; \
    fi

FROM debian:bookworm-slim
COPY --from=build /opt/arrow/lib /opt/arrow/lib
COPY --from=build /artifacts/ /usr/local/bin/
COPY --chmod=0755 scripts/entrypoint.sh /usr/local/bin/run-pipeline
ENV LD_LIBRARY_PATH=/opt/arrow/lib
USER 1000:1000
ENTRYPOINT ["/usr/local/bin/run-pipeline"]
