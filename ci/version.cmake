execute_process(COMMAND git describe --always --dirty --abbrev=40
  WORKING_DIRECTORY "${SOURCE}" OUTPUT_VARIABLE version
  OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  set(version "unknown")
endif()
set(content "#define GMUX_VERSION \"${version}\"\n")
if(EXISTS "${OUTPUT}")
  file(READ "${OUTPUT}" previous)
endif()
if(NOT content STREQUAL previous)
  file(WRITE "${OUTPUT}" "${content}")
endif()
