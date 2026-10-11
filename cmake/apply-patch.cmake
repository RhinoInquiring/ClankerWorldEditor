# cmake -DPATCH=<file> -P apply-patch.cmake, run in a fetched source tree: applies the patch once
# (a re-run of the patch step finds it already applied and leaves the tree alone).
execute_process(COMMAND git apply --reverse --check "${PATCH}" RESULT_VARIABLE notApplied OUTPUT_QUIET ERROR_QUIET)
if(notApplied)
    execute_process(COMMAND git apply "${PATCH}" RESULT_VARIABLE failed)
    if(failed)
        message(FATAL_ERROR "could not apply ${PATCH}")
    endif()
endif()
