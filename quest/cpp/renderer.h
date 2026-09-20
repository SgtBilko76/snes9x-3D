#pragma once

#include "emu.h"

#include <GLES3/gl3.h>

#include <string>

namespace renderer {

bool Init();
void Shutdown();

// Uploads a finished SNES frame, pixels and priority buffer both.
void UploadFrame(const emu::Frame &frame);

// Draws the uploaded frame into an OpenXR swapchain image.  `shift` is this
// eye's signed maximum horizontal displacement as a fraction of the image
// width, so it stays correct whatever the screen is resized to: zero renders
// the frame flat, and the two eyes take opposite signs for stereo.
void DrawEye(GLuint color_texture, int width, int height, float shift);

// Debug aid: writes the next two rendered eyes out as PPMs, so the stereo
// warp can be checked without a headset.
void RequestEyeDump(const std::string &path_prefix);

// The options menu is drawn on the CPU into an RGB565 buffer and shown as its
// own layer.
void UploadOverlay(const uint16_t *pixels, int width, int height);
void DrawOverlay(GLuint color_texture, int width, int height);

// 0 pixels, 1 sharp, 2 soft.
void SetFilter(int filter);

// Fills a swapchain image with a flat colour, for the backdrop layer.
void ClearSwapchainImage(GLuint color_texture, int width, int height,
                         float red, float green, float blue);

} // namespace renderer
