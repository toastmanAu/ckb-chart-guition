# Firmware scaffold intentionally non-compilable

Implementer must create main ESP-IDF project, pinned component dependencies and correct RGB/GT911 driver after confirming board revision. `contracts/light_client_adapter.h` is the proposed abstract boundary, not upstream API. Do not publish/flash a fake binary.
