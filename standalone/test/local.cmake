# Tests for transports that exist only in this tree.
#
# xlog_category_pico is unregistered: its only drivers, pico_evb_relay_server and
# pico_evb_text_client, build under BUILD_SAMPLES, which nothing sets. Restore it
# once moqtest speaks picoquic.
