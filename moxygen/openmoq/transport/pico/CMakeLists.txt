# Filter out non-existent picoquic libraries (e.g., picotls-fusion which may not be built)
set(FILTERED_PICOQUIC_LIBRARIES "")
foreach(lib ${picoquic_LIBRARIES})
  if(EXISTS "${lib}")
    list(APPEND FILTERED_PICOQUIC_LIBRARIES "${lib}")
  else()
    message(STATUS "Skipping non-existent picoquic library: ${lib}")
  endif()
endforeach()

moxygen_add_library(openmoq_pico_transport
  SRCS
    PicoQuicExecutor.cpp
    PicoQuicWebTransport.cpp
    MoQPicoQuicServer.cpp
  EXPORTED_DEPS
    moxygen_events_moq_executor
    proxygen::proxygen_http_webtransport
    proxygen::proxygen_http_webtransport_wt_stream_manager
    Folly::folly_function
    Folly::folly_logging_logging
    Folly::folly_network_address
    Folly::folly_string
    Folly::folly_coro_blocking_wait
    ${FILTERED_PICOQUIC_LIBRARIES}
)

target_include_directories(openmoq_pico_transport
  PUBLIC
    ${picoquic_INCLUDE_DIR}
)
