# Copyright (c) OpenMOQ contributors.
# This source code is licensed under the Apache 2.0 license found in the
# LICENSE file in the root directory of this source tree.

if(NOT BUILD_TESTS)
    return()
endif()

moxygen_add_test(TARGET PicoWebTransportBaseTests
  PREFIX "PicoWebTransportBase"
  SOURCES
    PicoQuicWebTransportTest.cpp
    PicoWebTransportBaseTest.cpp
  DEPENDS
    openmoq_pico_base
    testmain
)

moxygen_add_test(TARGET MoQPicoQuicShardedServerTests
  PREFIX "MoQPicoQuicShardedServer"
  SOURCES
    MoQPicoQuicShardedServerTest.cpp
  DEPENDS
    openmoq_pico_evb_sharded_server_transport
    moxygen_moqclient
    moxygen_util_insecure_verifier_dangerous_do_not_use_in_production
    OpenSSL::Crypto
    Folly::folly_coro_blocking_wait
    Folly::folly_coro_timeout
    Folly::folly_testing_test_util
    testmain
)
