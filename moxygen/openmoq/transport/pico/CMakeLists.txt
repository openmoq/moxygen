# Filter out non-existent picoquic libraries (e.g., picotls-fusion which may not be built)
set(FILTERED_PICOQUIC_LIBRARIES "")
foreach(lib ${picoquic_LIBRARIES})
  if(TARGET "${lib}" OR EXISTS "${lib}")
    list(APPEND FILTERED_PICOQUIC_LIBRARIES "${lib}")
  else()
    message(STATUS "Skipping non-existent picoquic library: ${lib}")
  endif()
endforeach()

moxygen_add_library(openmoq_pico_transport
  SRCS
    PicoQuicExecutor.cpp
    PicoQuicWebTransport.cpp
  EXPORTED_DEPS
    moxygen_events_moq_executor
    proxygen::proxygen_http_webtransport
    proxygen::proxygen_http_webtransport_wt_stream_manager
    mvfst::mvfst_priority_http_priority_queue
    Folly::folly_function
    Folly::folly_logging_logging
    Folly::folly_network_address
    ${FILTERED_PICOQUIC_LIBRARIES}
)

target_include_directories(openmoq_pico_transport
  PRIVATE
    ${picoquic_INCLUDE_DIR}
)
