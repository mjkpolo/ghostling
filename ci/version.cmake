if(DEFINED ENV{GITHUB_SHA})
  # The checkout in a CI container may have a different owner from Git's user.
  set(version "$ENV{GITHUB_SHA}")
else()
  execute_process(COMMAND git describe --always --dirty --abbrev=40
    WORKING_DIRECTORY "${SOURCE}" OUTPUT_VARIABLE version
    OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE status)
  if(NOT status EQUAL 0)
    set(version "unknown")
  endif()
endif()
set(content "#define GMUX_VERSION \"${version}\"\n")
if(EXISTS "${OUTPUT}")
  file(READ "${OUTPUT}" previous)
endif()
if(NOT content STREQUAL previous)
  file(WRITE "${OUTPUT}" "${content}")
endif()
