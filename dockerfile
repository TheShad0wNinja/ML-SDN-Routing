FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV LANG=C.UTF-8
ENV LC_ALL=C.UTF-8

# Install dependencies
RUN apt-get update && apt-get install -y \
    build-essential g++ gcc \
    python3 python3-dev python3-pip python3-setuptools \
    cmake ninja-build \
    git wget vim \
    pkg-config autoconf libtool \
    libboost-all-dev \
    libgsl-dev libgtk-3-dev \
    sqlite3 libsqlite3-dev \
    libxml2 libxml2-dev \
    libpcap-dev \
    tcpdump \
    libzmq3-dev \ 
    iproute2 uml-utilities bridge-utils \
    && rm -rf /var/lib/apt/lists/*

# Install Ryu with pinned compatible versions
RUN pip3 install --no-cache-dir \
    werkzeug==2.0.3 \
    eventlet==0.41.0 \
    dnspython==2.8.0 \
    ryu==4.34 \
    pyzmq==27.1.0 \ 
    torch \
    numpy

# Patch eventless
# RUN sed -i -e 's/from eventlet.wsgi import ALREADY_HANDLED/import eventlet.wsgi/g' -e 's/_ALREADY_HANDLED = ALREADY_HANDLED/_ALREADY_HANDLED = getattr(getattr(eventlet.wsgi, "WSGI_LOCAL", None), "already_handled", None)/g' $(python3 -c "import ryu.app.wsgi as w; print(w.__file__)")

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
RUN set -e
RUN patch -p1 < contrib/ofswitch13/utils/ofswitch13-3_40.patch
RUN patch -p1 < contrib/ofswitch13/utils/csma-full-duplex-3_40.patch

# Make HandleEchoReply virtual in base controller
RUN python3 -c "\
import pathlib;\
p = pathlib.Path('/workspace/ns-3.40/contrib/ofswitch13/model/ofswitch13-controller.h');\
c = p.read_text();\
c = c.replace('    ofl_err HandleEchoReply(', '    virtual ofl_err HandleEchoReply(');\
p.write_text(c);\
print('Patched ofswitch13-controller.h')\
"

# Inject real ns-3 DataRate into OpenFlow port speed fields 
RUN  python3 -c "path='/workspace/ns-3.40/contrib/ofswitch13/model/ofswitch13-port.cc'; old='    m_swPort->conf->curr_speed = port_speed(m_swPort->conf->curr);\n    m_swPort->conf->max_speed = port_speed(m_swPort->conf->supported);'; new='    m_swPort->conf->curr_speed = port_speed(m_swPort->conf->curr);\n    m_swPort->conf->max_speed = port_speed(m_swPort->conf->supported);\n\n    // Patch: override with actual ns-3 DataRate in kbps.\n    DataRate rate;\n    Ptr<Channel> channel = m_netDev->GetChannel();\n    if (channel)\n      {\n        Ptr<CsmaChannel> csmaChannel = channel->GetObject<CsmaChannel>();\n        if (csmaChannel)\n          {\n            DataRateValue drv;\n            csmaChannel->GetAttribute(\"DataRate\", drv);\n            rate = drv.Get();\n          }\n      }\n    if (rate.GetBitRate() > 0)\n      {\n        m_swPort->conf->curr_speed = static_cast<uint32_t>(rate.GetBitRate() / 1000);\n        m_swPort->conf->max_speed = static_cast<uint32_t>(rate.GetBitRate() / 1000);\n      }'; f=open(path); c=f.read(); f.close(); c=c.replace(old,new); f=open(path,'w'); f.write(c); f.close()"

# Configure and build
RUN ./ns3 configure --disable-examples --disable-tests
RUN ./ns3 build

WORKDIR /workspace/ns-3.40/scratch
ENV NS3_DIR=/workspace/ns-3.40

CMD ["/bin/bash"]