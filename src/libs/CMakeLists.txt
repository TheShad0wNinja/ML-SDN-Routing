add_library(
  scratch_zmq_openflow STATIC
  zmq-openflow-controller.cc
  topology.cc
  openflow_builders.cc
)

target_include_directories(scratch_zmq_openflow PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(
  scratch_zmq_openflow PUBLIC "${ns3-libs}" "${ns3-contrib-libs}" zmq
)
