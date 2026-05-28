add_library(
  scratch_zmq_openflow STATIC
  controller/zmq-openflow-controller.cc
  controller/topology.cc
  controller/openflow_builders.cc
  scenario/scenario_topo.cc
  scenario/scenario_builder.cc
  scenario/scenario_traffic.cc
  scenario/scenario_stress.cc
  scenario/scenario_ml.cc
  scenario/scenario_multictrl.cc
  scenario/scenario_cli.cc
  scenario/scenario_runner.cc
  scenario/scenario_report.cc
)

target_include_directories(scratch_zmq_openflow PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(
  scratch_zmq_openflow PUBLIC "${ns3-libs}" "${ns3-contrib-libs}" zmq
)
