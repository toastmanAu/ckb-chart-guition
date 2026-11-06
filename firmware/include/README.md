# Firmware scaffold intentionally non-compilable

Implementer must create main ESP-IDF project, pinned component dependencies and correct RGB/GT911 driver after confirming board revision. The light-client boundary is `firmware/main/lc_task.h` (the original proposed `contracts/light_client_adapter.h` was removed once the real client was integrated). Do not publish/flash a fake binary.
