#pragma once

#include <functional>

#include "cpprest/json.h"
#include "cpprest/http_msg.h"

using namespace web::http;

void SendAndroidNotification( utility::string_t ServerKey, utility::string_t TargetUser, utility::string_t MessageText, utility::string_t CameraName, utility::string_t ImageUri, std::function<void(http_response)> OnComplete );
