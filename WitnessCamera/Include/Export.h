#pragma once

#if defined(_WIN32)
# if defined(WITNESSCAMERA_EXPORTS)
#  define CAMERA_API __declspec(dllexport)
# else
#  define CAMERA_API __declspec(dllimport)
# endif
#else
# if defined(WITNESSCAMERA_EXPORTS)
#  define CAMERA_API __attribute__((visibility("default")))
# else
#  define CAMERA_API
# endif
#endif
