FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV LANG=C.UTF-8
ENV LC_ALL=C.UTF-8

# Install dependencies
RUN apt-get update && apt-get install -y \
    build-essential g++ gcc \
    python3 python3-dev python3-pip \
    cmake ninja-build \
    git wget vim \
    pkg-config autoconf libtool \
    libboost-all-dev \
    libgsl-dev libgtk-3-dev \
    sqlite3 libsqlite3-dev \
    libxml2 libxml2-dev \
    tcpdump \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

# Clone ns-3.40
RUN git clone https://gitlab.com/nsnam/ns-3-dev.git ns-3.40 \
    && cd ns-3.40 \
    && git checkout ns-3.40

# Clone OFSwitch13
WORKDIR /workspace/ns-3.40/contrib
RUN git clone --recurse-submodules https://github.com/ljerezchaves/ofswitch13.git \
    && cd ofswitch13 \
    && git checkout 5.2.3 \
    && git submodule update --recursive

# Apply ns-3 patch for OpenFlow callback
WORKDIR /workspace/ns-3.40
RUN set -e; \
    patch -p1 < contrib/ofswitch13/utils/ofswitch13-3_40.patch 2>/dev/null || \
    patch -p1 < contrib/ofswitch13/utils/ofswitch13-3_39.patch

# Configure and build
RUN ./ns3 configure --enable-examples --enable-tests
RUN ./ns3 build

WORKDIR /workspace/ns-3.40/scratch
ENV NS3_DIR=/workspace/ns-3.40

CMD ["/bin/bash"]
