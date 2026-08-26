/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/Expected.h>
#include "moxygen/MoQFramer.h"
#include "moxygen/moqtest/Types.h"

namespace moxygen {

// Fixed extension type for the send timestamp (milliseconds since epoch).
// Even type => integer extension. Value is large enough to avoid collision with
// test integer extensions (which use 2 * testIntegerExtension).
constexpr uint64_t kTimestampExtensionType = 0xC000;

// Bytes of send timestamp written at the head of each test payload.
constexpr size_t kPayloadTimestampBytes = 8;

// Upper bound on a latency sample. Guards against an unstamped payload, whose
// first bytes decode to an enormous value, and against a clock step.
constexpr uint64_t kMaxPlausibleLatencyMs = 600000;

// Build a test payload of `size` bytes, writing the send time as big-endian
// milliseconds into the first 8 when `stamp` is set and the object is large
// enough to hold it.
//
// Payload bytes are opaque to a relay, so a timestamp carried here survives
// hops that drop object extensions they do not recognise - which is the case
// for at least one relay under test, whose end-to-end latency is otherwise
// unmeasurable. The extension remains the preferred carrier where it survives.
std::string makeTestPayload(size_t size, bool stamp);

folly::Expected<folly::Unit, std::runtime_error> validateMoQTestParameters(
    const MoQTestParameters& track);

folly::Expected<moxygen::TrackNamespace, std::runtime_error>
convertMoqTestParamToTrackNamespace(const MoQTestParameters& params);

folly::Expected<moxygen::MoQTestParameters, std::runtime_error>
convertTrackNamespaceToMoqTestParam(TrackNamespace* track);

std::vector<Extension> getExtensions(
    int integerExtensionId,
    int variableExtensionId,
    bool includeTimestamp = false);

int getObjectSize(uint64_t objectId, MoQTestParameters* params);

bool validatePayload(int objectSize, std::string payload);

bool validateExtensionSize(
    std::vector<Extension> extensions,
    MoQTestParameters* params);
bool validateIntExtensions(Extension intExt, MoQTestParameters* params);
bool validateVarExtensions(Extension varExt, MoQTestParameters* params);

} // namespace moxygen
