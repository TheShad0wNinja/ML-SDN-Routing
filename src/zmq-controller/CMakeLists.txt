build_exec(
  EXECNAME main-zmq
  EXECNAME_PREFIX scratch_
  SOURCE_FILES main-zmq.cc zmq-openflow-controller.cc
  LIBRARIES_TO_LINK "${ns3-libs}" "${ns3-contrib-libs}" zmq
)
