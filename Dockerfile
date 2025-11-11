FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
COPY . /work

# Ensure scripts are executable in the image
RUN chmod +x scripts/*.sh || true

# Build
RUN make all

# Default command shows help
CMD ["bash", "-lc", "build/mq_benchmark --help || true"]


