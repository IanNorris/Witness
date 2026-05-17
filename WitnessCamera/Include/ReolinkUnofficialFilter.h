#pragma once

#include "RecordFilterBase.h"
#include "ReolinkBaichuanClient.h"

#include <memory>
#include <string>

namespace Witness {
namespace Camera {

struct ReolinkUnofficialFilterData;

// Motion filter that uses Reolink camera's built-in AI detection via the
// unofficial Baichuan protocol (port 9000). Instead of analyzing video frames
// for motion, this filter queries the camera's own detection results.
//
// Named "reolink_unofficial" to clearly indicate this is community-researched,
// not endorsed by or affiliated with Reolink Technology Co., Ltd.
class CAMERA_API ReolinkUnofficialFilter : public RecordFilterBase<ReolinkUnofficialFilterData>
{
public:
	ReolinkUnofficialFilter(
		const MotionChainNode& Chain,
		const std::string& host,
		int port,
		const std::string& username,
		const std::string& password
	);

	virtual ~ReolinkUnofficialFilter();

	virtual bool ProcessFrame(SharedClassificationTask TaskData) override;
	virtual void ClearStateThis() override;

	bool IsConnected() const;

	// Access the underlying Baichuan client (for PTZ commands)
	std::shared_ptr<ReolinkBaichuanClient> GetClient() const { return m_Client; }

private:
	std::shared_ptr<ReolinkBaichuanClient> m_Client;
};

}}
