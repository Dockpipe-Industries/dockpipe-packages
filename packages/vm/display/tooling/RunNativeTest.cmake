# Execute each test once, preserving its own output and original child status.
if(NOT DEFINED PROGRAM OR NOT EXISTS "${PROGRAM}")
    message(FATAL_ERROR "Native test executable is missing")
endif()
execute_process(COMMAND "${PROGRAM}" RESULT_VARIABLE child_status)
if(NOT child_status STREQUAL "0")
    message(FATAL_ERROR "Native test child exit status: ${child_status}")
endif()
