# Warnings as errors for project targets only (not third-party code).
function(recs_set_warnings target)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Werror)
endfunction()
