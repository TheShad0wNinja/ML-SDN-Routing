get_filename_component(_scenario_src_dir ${CMAKE_CURRENT_SOURCE_DIR} ABSOLUTE)
string(REPLACE "${PROJECT_SOURCE_DIR}" "${CMAKE_OUTPUT_DIRECTORY}"
               _scenario_out_dir ${_scenario_src_dir}
)

build_exec(
  EXECNAME two-switch-ping
  SOURCE_FILES main.cc
  LIBRARIES_TO_LINK scratch_zmq_openflow
  EXECUTABLE_DIRECTORY_PATH ${_scenario_out_dir}/
)
