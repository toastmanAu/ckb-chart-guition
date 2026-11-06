#include "light_client_adapter.h"

class EmbeddedFilterAdapter final : public ILightClientAdapter {
public:
    bool begin(const char *network) override
    {
        (void)network;
        status_ = {VerifyState::UNAVAILABLE, 0, 0, 0, 0, 0};
        return false;
    }

    void poll(uint32_t budget_ms) override { (void)budget_ms; }

    LightClientStatus status() const override { return status_; }

    bool watch(const WatchedScript &script) override
    {
        (void)script;
        return false;
    }

    void stop() override
    {
        status_.state = VerifyState::UNAVAILABLE;
    }

private:
    LightClientStatus status_{VerifyState::UNAVAILABLE, 0, 0, 0, 0, 0};
};

extern "C" ILightClientAdapter *embedded_light_client_adapter(void)
{
    static EmbeddedFilterAdapter adapter;
    return &adapter;
}
