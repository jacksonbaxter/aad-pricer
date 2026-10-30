# Linked PRIVATE by the helpers in AadpTargets, so third-party code never sees it.
add_library(aadp_warnings INTERFACE)
target_compile_options(aadp_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror)
