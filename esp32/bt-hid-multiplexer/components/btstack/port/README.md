# BTstack ESP32 port glue

Copied unmodified from BTstack `port/esp32/components/btstack/` at v1.6.2
(commit `501e6d2b86e6c92bfb9c390bcf55709938e25ac1`, the copy inside `~/pico-sdk/lib/btstack`):

- `btstack_port_esp32.c`, `btstack_port_esp32.h`: VHCI transport, `btstack_init()`
- `btstack_tlv_esp32.c`, `btstack_tlv_esp32.h`: BTstack TLV on NVS (namespace `BTstack`)

They are copied rather than referenced because that directory's `include/` also holds BTstack's stock
`btstack_config.h`, which must not be on the include path. When updating BTstack, copy them again
and re-apply any local changes (none so far).
