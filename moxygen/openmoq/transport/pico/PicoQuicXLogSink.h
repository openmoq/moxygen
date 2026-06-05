/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// Forward-decl to keep this header light. The C type comes from picoquic.h.
extern "C" {
typedef struct st_picoquic_quic_t picoquic_quic_t;
}

namespace moxygen::openmoq::pico {

/**
 * Install a folly XLOG sink as picoquic's text-log backend on the given quic
 * context. After this call, picoquic's internal log events (per-packet, per-cnx
 * lifecycle, drops, losses, CC dumps, ALPN negotiation, etc.) are dispatched
 * through folly's LoggerDB and governed by the standard --logging=... config
 * string under the `quic.picoquic.*` category root.
 *
 * Mutually exclusive with picoquic_set_textlog() — both target the same slot
 * (picoquic_quic_t::text_log_fns). qlog (picoquic_set_qlog) is unaffected and
 * can run in parallel as a separate channel; binlog (picoquic_set_binlog) the
 * same.
 *
 * The sink is registered with a static struct that has process-lifetime
 * storage, so this function may be called multiple times safely. To
 * uninstall, set quic->text_log_fns = nullptr through whatever API picoquic
 * provides (typically picoquic_textlog_close).
 *
 * Severity mapping summary:
 *   INFO    — app_message hooks (deliberate app-level logs)
 *   DBG1    — connection lifecycle, ALPN, drops, losses
 *   DBG2    — transport params, TLS ticket, CC dump
 *   DBG3    — per-packet (pdu in/out, decrypted packet, outgoing packet)
 *
 * Operators target the bridge via the standard XLOG config:
 *   --logging=quic.picoquic=INFO         // lifecycle + app msgs
 *   --logging=quic.picoquic=DBG1         // adds drops/losses
 *   --logging=quic.picoquic=DBG3         // adds per-packet firehose
 *
 * @param quic  the picoquic context to install the sink on. Must be non-null.
 */
void installPicoQuicXLogSink(picoquic_quic_t* quic);

} // namespace moxygen::openmoq::pico
