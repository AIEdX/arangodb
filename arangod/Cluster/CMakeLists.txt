add_library(arango_cluster_methods STATIC
  ClusterMethods.cpp
  ClusterTtlMethods.cpp)

target_link_libraries(arango_cluster_methods
  arango
  arango_network
  zlib_interface)

target_include_directories(arango_cluster_methods PRIVATE
  "${PROJECT_SOURCE_DIR}/arangod"
  "${PROJECT_SOURCE_DIR}/${ENTERPRISE_INCLUDE_DIR}")
