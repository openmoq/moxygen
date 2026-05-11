# Copyright (c) Synamedia
# SPDX-License-Identifier: Apache-2.0

# moqswitchpub — synthetic publisher for SWITCH testing
add_executable(
  moqswitchpub
  MoQSwitchPub.cpp
)
set_target_properties(
  moqswitchpub
  PROPERTIES
    BUILD_RPATH ${DEPS_LIBRARIES_DIR}
    INSTALL_RPATH ${DEPS_LIBRARIES_DIR}
)
target_include_directories(
  moqswitchpub PUBLIC
  $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
)
target_compile_options(
  moqswitchpub PRIVATE
  ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
)
target_link_libraries(
  moqswitchpub PUBLIC
  moxygen_events_moq_folly_executor_impl
  moxygen_moq_webtransport_client
  moxygen_relay_moq_forwarder
  moxygen_relay_moq_relay_client
  Folly::folly_init_init
  Folly::folly_futures_core
  Folly::folly_io_async_scoped_event_base_thread
)

# moqswitchsub — subscriber that sends SWITCH and validates relay response
add_executable(
  moqswitchsub
  MoQSwitchSub.cpp
)
set_target_properties(
  moqswitchsub
  PROPERTIES
    BUILD_RPATH ${DEPS_LIBRARIES_DIR}
    INSTALL_RPATH ${DEPS_LIBRARIES_DIR}
)
target_include_directories(
  moqswitchsub PUBLIC
  $<BUILD_INTERFACE:${MOXYGEN_FBCODE_ROOT}>
  # SwitchTypes.h lives in the moqx src tree, not in the moxygen submodule
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../src
)
target_compile_options(
  moqswitchsub PRIVATE
  ${_MOXYGEN_COMMON_COMPILE_OPTIONS}
  # GCC 12 ICEs in morph_fn_to_coro on coroutines that co_await types
  # containing std::string (e.g. folly::Expected<T, RequestError>).
  # -O0 disables the tree-sink / SSA passes that trigger the bug.
  -O0
)
target_link_libraries(
  moqswitchsub PUBLIC
  moxygen_events_moq_folly_executor_impl
  moxygen_moq_consumers
  moxygen_moq_webtransport_client
  moxygen_relay_moq_relay_client
  mvfst::mvfst_folly_utils
  Folly::folly_init_init
  Folly::folly_json_dynamic
  Folly::folly_futures_core
  Folly::folly_io_async_scoped_event_base_thread
)

install(
    TARGETS moqswitchpub moqswitchsub
    EXPORT moxygen-exports
    ARCHIVE DESTINATION ${LIB_INSTALL_DIR}
    LIBRARY DESTINATION ${LIB_INSTALL_DIR}
)
