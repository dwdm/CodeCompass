include_directories(
  include
  ${PROJECT_SOURCE_DIR}/util/include
  ${PROJECT_SOURCE_DIR}/webserver/include
  ${PROJECT_SOURCE_DIR}/model/include
  ${PROJECT_SOURCE_DIR}/service/language/gen-cpp
  ${PROJECT_SOURCE_DIR}/service/project/gen-cpp
  ${PROJECT_SOURCE_DIR}/service/project/include
  ${PLUGIN_DIR}/model/include
  ${THRIFT_LIBTHRIFT_INCLUDE_DIRS})

add_library(cppservice SHARED
  src/cppservice.cpp
  src/plugin.cpp
  src/diagram.cpp)

target_compile_options(cppservice PUBLIC -Wno-unknown-pragmas)

target_link_libraries(cppservice
  util
  model
  cppmodel
  mongoose
  projectservice
  languagethrift
  gvc
  ${THRIFT_LIBTHRIFT_LIBRARIES})

install(TARGETS cppservice DESTINATION ${INSTALL_SERVICE_DIR})
