#pragma once
#include <memory>
#include <string>
#include <vector>
#include "ReolinkClient.h"

// Local ONVIF Profile S transport. Owned credentials, no database or context
// references; retained safely by movement expiry and shutdown callbacks.
class OnvifPtzClient
{
public:
    static std::shared_ptr<OnvifPtzClient> Get(const std::string& host, int port,
        const std::string& username, const std::string& password);
    ~OnvifPtzClient();
    bool Move(PtzOp op, int speed, ReolinkClient::SendAllowed allowed);
    bool Stop();
    bool SupportsPanTilt();
    std::vector<PtzPreset> Presets();
    bool GoToPreset(int id, ReolinkClient::SendAllowed allowed);
    std::string Error() const;
    std::string ProfileEvidence() const;
private:
    OnvifPtzClient(const std::string&, int, const std::string&, const std::string&);
    struct Impl;
    std::unique_ptr<Impl> State;
};
