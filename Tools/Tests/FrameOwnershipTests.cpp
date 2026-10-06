#include "FFMPEG/Frame.h"
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>

using Witness::Camera::FFMPEG::Frame;

static void Require(bool Condition, const char* Message) {
    if (!Condition) throw std::runtime_error(Message);
}

static void CheckOwnedPlane(AVFrame* Value) {
    AVBufferRef* Buffer = av_frame_get_plane_buffer(Value, 0);
    Require(Buffer != nullptr, "plane has no reference-counted owner");
    const auto Begin = reinterpret_cast<uintptr_t>(Buffer->data);
    const auto Data = reinterpret_cast<uintptr_t>(Value->data[0]);
    Require(Data >= Begin && Data - Begin < Buffer->size,
            "plane data is outside its reference-counted buffer");
    const auto Used = static_cast<size_t>(Value->linesize[0]) * Value->height;
    Require(Used <= Buffer->size - (Data - Begin), "plane exceeds its owner");
}

static void Check(int Width, int Height, int Alignment) {
    Frame Value(Width, Height, AV_PIX_FMT_BGR24, Alignment);
    Value.Prepare();
    CheckOwnedPlane(Value.GetFrame());
    Require(Value.GetFrame()->linesize[0] >= Width * 3, "short stride");
    Value.GetFrame()->data[0][0] = 17;
    AVFrame* Reference = av_frame_clone(Value.GetFrame());
    Require(Reference != nullptr, "clone failed");
    try {
        Value.Prepare(); // writable output must not overwrite a retained frame
        CheckOwnedPlane(Value.GetFrame());
        Value.GetFrame()->data[0][0] = 29;
        Require(Reference->data[0][0] == 17, "retained frame was overwritten");
        Value.Unref();
        Require(Reference->data[0][0] == 17, "unref invalidated retained pixels");
        Value.Prepare(); // dimensions/format must be restored after Unref
        CheckOwnedPlane(Value.GetFrame());
        Require(Value.GetFrame()->width == Width && Value.GetFrame()->height == Height,
                "prepare after unref lost dimensions");
    } catch (...) {
        av_frame_free(&Reference);
        throw;
    }
    av_frame_free(&Reference);
}

static void CheckScaling() {
    Frame Input(160, 90, AV_PIX_FMT_YUV420P);
    Frame Output(711, 400, AV_PIX_FMT_BGR24);
    Input.Prepare();
    Output.Prepare();
    for (int Plane = 0; Plane < 3; ++Plane) {
        const int Width = Plane == 0 ? 160 : 80;
        const int Height = Plane == 0 ? 90 : 45;
        for (int Row = 0; Row < Height; ++Row)
            std::memset(Input.GetFrame()->data[Plane] + Row * Input.GetFrame()->linesize[Plane],
                        Plane == 0 ? 16 : 128, Width);
    }
    SwsContext* Context = sws_getContext(160, 90, AV_PIX_FMT_YUV420P,
        711, 400, AV_PIX_FMT_BGR24, SWS_BILINEAR, nullptr, nullptr, nullptr);
    Require(Context != nullptr, "scaler allocation failed");
    const int Rows = sws_scale(Context, Input.GetFrame()->data, Input.GetFrame()->linesize,
        0, 90, Output.GetFrame()->data, Output.GetFrame()->linesize);
    sws_freeContext(Context);
    Require(Rows == 400, "scaler did not fill output");
    CheckOwnedPlane(Output.GetFrame());
    // This size has padded output rows. Read the final pixel with the actual
    // FFmpeg stride, not an assumed width*3 tightly packed row layout.
    const auto* Last = Output.GetFrame()->data[0] + 399 * Output.GetFrame()->linesize[0] + 710 * 3;
    Require(Last[0] == 0 && Last[1] == 0 && Last[2] == 0, "scaled pixels/stride mismatch");
}

int main() {
    try {
        for (int Width : {1, 159, 160, 711, 1280})
            for (int Alignment : {0, 1, 32}) Check(Width, 90, Alignment);
        CheckScaling();
        std::cout << "Frame ownership, writable reuse and retained references passed\n";
        return 0;
    } catch (const std::exception& Error) {
        std::cerr << Error.what() << '\n';
        return 1;
    }
}
