# cmake -DCXX=<compiler> -DFLAGS=<a|b|c> -DCASES=<file|file> -DSTAMP=<file> -P must-not-compile.cmake
#
# For each case: compiled as written it must succeed, and with -DVIOLATE it must fail
# with an error matching the regex on its first line ("// expect: <regex>"). The control
# compile proves the failure comes from the violation, not from a broken include path.

string(REPLACE "|" ";" flags "${FLAGS}")
string(REPLACE "|" ";" cases "${CASES}")

list(LENGTH cases count)
if(count EQUAL 0)
    message(FATAL_ERROR "must-not-compile: no cases found")
endif()

foreach(case IN LISTS cases)
    get_filename_component(name "${case}" NAME)
    file(STRINGS "${case}" first LIMIT_COUNT 1)
    string(REGEX REPLACE "^// expect: " "" expect "${first}")
    if(expect STREQUAL first OR expect STREQUAL "")
        message(FATAL_ERROR "must-not-compile: ${name} has no '// expect: <regex>' first line")
    endif()

    execute_process(COMMAND "${CXX}" ${flags} "${case}" RESULT_VARIABLE control_rc ERROR_VARIABLE control_err)
    if(NOT control_rc EQUAL 0)
        message(FATAL_ERROR "must-not-compile: ${name} control does not compile:\n${control_err}")
    endif()

    execute_process(COMMAND "${CXX}" ${flags} -DVIOLATE "${case}" RESULT_VARIABLE rc ERROR_VARIABLE err)
    if(rc EQUAL 0)
        message(FATAL_ERROR "must-not-compile: ${name} compiled with -DVIOLATE, and it must not")
    endif()
    if(NOT err MATCHES "${expect}")
        message(FATAL_ERROR "must-not-compile: ${name} failed, but not with '${expect}':\n${err}")
    endif()
    message(STATUS "must-not-compile: ${name} refused as expected")
endforeach()

file(WRITE "${STAMP}" "${count} cases\n")
