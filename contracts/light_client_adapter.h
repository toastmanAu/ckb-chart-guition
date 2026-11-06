#pragma once
// Interface specification only. This is not a compiled, operational light-client implementation.
#include <stdint.h>
#include <stddef.h>

enum class VerifyState : uint8_t { UNAVAILABLE, CONNECTING, SYNCING, VERIFIED, DEGRADED, ERROR };
struct LightClientStatus {
  VerifyState state;
  uint64_t verified_tip_height;
  uint32_t peer_count;
  uint32_t filter_matches;
  uint32_t filter_errors;
  uint64_t updated_ms;
};
struct WatchedScript {
  const char* code_hash_hex;
  const char* hash_type;   // 'type' or 'data'/data1 as supported by protocol
  const char* args_hex;
  uint64_t start_height;
};
class ILightClientAdapter {
public:
  virtual ~ILightClientAdapter() = default;
  virtual bool begin(const char* network) = 0;
  virtual void poll(uint32_t budget_ms) = 0;
  virtual LightClientStatus status() const = 0;
  virtual bool watch(const WatchedScript& script) = 0;
  virtual void stop() = 0;
};
