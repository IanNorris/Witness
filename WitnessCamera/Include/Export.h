#pragma once

#if defined(WITNESSCAMERA_EXPORTS)
# define CAMERA_API __declspec(dllexport)
#else
# define CAMERA_API __declspec(dllimport)
#endif
