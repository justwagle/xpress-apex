# Make sure the compiler can find include files from our Apex library. 
include_directories (${APEX_SOURCE_DIR}/src/apex
${APEX_SOURCE_DIR}/src/wrappers)

# Make sure the linker can find the Apex library once it is built. 
link_directories (${APEX_BINARY_DIR}/src/apex)

# Add library called "testOverhead" that is built from the source file
add_library (apex_pthread_wrapper pthread_wrapper.c pthread_wrapper_internal.cpp)
add_dependencies (apex_pthread_wrapper apex)
target_link_libraries (apex_pthread_wrapper apex)

INSTALL(TARGETS apex_pthread_wrapper 
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
)
