# Tests for transports that exist only in this tree.
#
# xlog_category_pico is unregistered: its only driver pair, pico_evb_relay_server
# and pico_evb_text_client, is built under BUILD_SAMPLES, which the artifact and
# test builds no longer set. Restore it once moqtest speaks picoquic (#377).
