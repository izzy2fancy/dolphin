// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/Present.h"

#include "Core/HW/VideoInterface.h"
#include "Core/Host.h"

#include "InputCommon/ControllerInterface/ControllerInterface.h"

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/FrameDumper.h"
#include "VideoCommon/OnScreenUI.h"
#include "VideoCommon/PostProcessing.h"
#include "VideoCommon/RenderBase.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoConfig.h"

std::unique_ptr<VideoCommon::Presenter> g_presenter;

namespace VideoCommon
{
static float AspectToWidescreen(float aspect)
{
  return aspect * ((16.0f / 9.0f) / (4.0f / 3.0f));
}

Presenter::Presenter()
{
}

Presenter::~Presenter()
{
  // Disable ControllerInterface's aspect ratio adjustments so mapping dialog behaves normally.
  g_controller_interface.SetAspectRatioAdjustment(1);
}

bool Presenter::Initialize()
{
  UpdateDrawRectangle();

  m_post_processor = std::make_unique<VideoCommon::PostProcessing>();
  if (!m_post_processor->Initialize(m_backbuffer_format))
    return false;

  m_onscreen_ui = std::make_unique<OnScreenUI>();
  if (!m_onscreen_ui->Initialize(m_backbuffer_width, m_backbuffer_height, m_backbuffer_scale))
    return false;

  if (!g_gfx->IsHeadless())
    SetBackbuffer(g_gfx->GetSurfaceInfo());

  return true;
}

void Presenter::SetBackbuffer(int backbuffer_width, int backbuffer_height)
{
  m_backbuffer_width = backbuffer_width;
  m_backbuffer_height = backbuffer_height;
  UpdateDrawRectangle();
}

void Presenter::SetBackbuffer(SurfaceInfo info)
{
  m_backbuffer_width = info.width;
  m_backbuffer_height = info.height;
  m_backbuffer_scale = info.scale;
  m_backbuffer_format = info.format;
  UpdateDrawRectangle();
}

void Presenter::CheckForConfigChanges(u32 changed_bits)
{
  // Check for post-processing shader changes. Done up here as it doesn't affect anything outside
  // the post-processor. Note that options are applied every frame, so no need to check those.
  if (m_post_processor->GetConfig()->GetShader() != g_ActiveConfig.sPostProcessingShader)
  {
    // The existing shader must not be in use when it's destroyed
    g_gfx->WaitForGPUIdle();

    m_post_processor->RecompileShader();
  }

  // Stereo mode change requires recompiling our post processing pipeline and imgui pipelines for
  // rendering the UI.
  if (changed_bits & ConfigChangeBits::CONFIG_CHANGE_BIT_STEREO_MODE)
  {
    m_onscreen_ui->RecompileImGuiPipeline();
    m_post_processor->RecompilePipeline();
  }
}

void Presenter::BeginUIFrame()
{
  if (g_gfx->IsHeadless())
    return;

  g_gfx->BeginUtilityDrawing();
  g_gfx->BindBackbuffer({0.0f, 0.0f, 0.0f, 1.0f});
}

void Presenter::EndUIFrame()
{
  m_onscreen_ui->Finalize();

  if (g_gfx->IsHeadless())
  {
    m_onscreen_ui->DrawImGui();

    std::lock_guard<std::mutex> guard(m_swap_mutex);
    g_gfx->PresentBackbuffer();
    g_gfx->EndUtilityDrawing();
  }

  m_onscreen_ui->BeginImGuiFrame(m_backbuffer_width, m_backbuffer_height);
}

std::tuple<MathUtil::Rectangle<int>, MathUtil::Rectangle<int>>
Presenter::ConvertStereoRectangle(const MathUtil::Rectangle<int>& rc) const
{
  // Resize target to half its original size
  auto draw_rc = rc;
  if (g_ActiveConfig.stereo_mode == StereoMode::TAB)
  {
    // The height may be negative due to flipped rectangles
    int height = rc.bottom - rc.top;
    draw_rc.top += height / 4;
    draw_rc.bottom -= height / 4;
  }
  else
  {
    int width = rc.right - rc.left;
    draw_rc.left += width / 4;
    draw_rc.right -= width / 4;
  }

  // Create two target rectangle offset to the sides of the backbuffer
  auto left_rc = draw_rc;
  auto right_rc = draw_rc;
  if (g_ActiveConfig.stereo_mode == StereoMode::TAB)
  {
    left_rc.top -= m_backbuffer_height / 4;
    left_rc.bottom -= m_backbuffer_height / 4;
    right_rc.top += m_backbuffer_height / 4;
    right_rc.bottom += m_backbuffer_height / 4;
  }
  else
  {
    left_rc.left -= m_backbuffer_width / 4;
    left_rc.right -= m_backbuffer_width / 4;
    right_rc.left += m_backbuffer_width / 4;
    right_rc.right += m_backbuffer_width / 4;
  }

  return std::make_tuple(left_rc, right_rc);
}

float Presenter::CalculateDrawAspectRatio() const
{
  const auto aspect_mode = g_ActiveConfig.aspect_mode;

  // If stretch is enabled, we prefer the aspect ratio of the window.
  if (aspect_mode == AspectMode::Stretch)
    return (static_cast<float>(m_backbuffer_width) / static_cast<float>(m_backbuffer_height));

  const float aspect_ratio = VideoInterface::GetAspectRatio();

  if (aspect_mode == AspectMode::AnalogWide ||
      (aspect_mode == AspectMode::Auto && g_renderer->IsGameWidescreen()))
  {
    return AspectToWidescreen(aspect_ratio);
  }

  return aspect_ratio;
}

void Presenter::AdjustRectanglesToFitBounds(MathUtil::Rectangle<int>* target_rect,
                                            MathUtil::Rectangle<int>* source_rect, int fb_width,
                                            int fb_height)
{
  const int orig_target_width = target_rect->GetWidth();
  const int orig_target_height = target_rect->GetHeight();
  const int orig_source_width = source_rect->GetWidth();
  const int orig_source_height = source_rect->GetHeight();
  if (target_rect->left < 0)
  {
    const int offset = -target_rect->left;
    target_rect->left = 0;
    source_rect->left += offset * orig_source_width / orig_target_width;
  }
  if (target_rect->right > fb_width)
  {
    const int offset = target_rect->right - fb_width;
    target_rect->right -= offset;
    source_rect->right -= offset * orig_source_width / orig_target_width;
  }
  if (target_rect->top < 0)
  {
    const int offset = -target_rect->top;
    target_rect->top = 0;
    source_rect->top += offset * orig_source_height / orig_target_height;
  }
  if (target_rect->bottom > fb_height)
  {
    const int offset = target_rect->bottom - fb_height;
    target_rect->bottom -= offset;
    source_rect->bottom -= offset * orig_source_height / orig_target_height;
  }
}

void Presenter::ReleaseXFBContentLock()
{
  if (m_xfb_entry)
    m_xfb_entry->ReleaseContentLock();
}

void Presenter::ChangeSurface(void* new_surface_handle)
{
  std::lock_guard<std::mutex> lock(m_swap_mutex);
  m_new_surface_handle = new_surface_handle;
  m_surface_changed.Set();
}

void Presenter::ResizeSurface()
{
  std::lock_guard<std::mutex> lock(m_swap_mutex);
  m_surface_resized.Set();
}

void* Presenter::GetNewSurfaceHandle()
{
  return m_new_surface_handle;
  m_new_surface_handle = nullptr;
}

void Presenter::SetWindowSize(int width, int height)
{
  const auto [out_width, out_height] = g_presenter->CalculateOutputDimensions(width, height);

  // Track the last values of width/height to avoid sending a window resize event every frame.
  if (out_width == m_last_window_request_width && out_height == m_last_window_request_height)
    return;

  m_last_window_request_width = out_width;
  m_last_window_request_height = out_height;
  Host_RequestRenderWindowSize(out_width, out_height);
}

// Crop to exactly 16:9 or 4:3 if enabled and not AspectMode::Stretch.
std::tuple<float, float> Presenter::ApplyStandardAspectCrop(float width, float height) const
{
  const auto aspect_mode = g_ActiveConfig.aspect_mode;

  if (!g_ActiveConfig.bCrop || aspect_mode == AspectMode::Stretch)
    return {width, height};

  // Force 4:3 or 16:9 by cropping the image.
  const float current_aspect = width / height;
  const float expected_aspect =
      (aspect_mode == AspectMode::AnalogWide ||
       (aspect_mode == AspectMode::Auto && g_renderer->IsGameWidescreen())) ?
          (16.0f / 9.0f) :
          (4.0f / 3.0f);
  if (current_aspect > expected_aspect)
  {
    // keep height, crop width
    width = height * expected_aspect;
  }
  else
  {
    // keep width, crop height
    height = width / expected_aspect;
  }

  return {width, height};
}

void Presenter::UpdateDrawRectangle()
{
  const float draw_aspect_ratio = CalculateDrawAspectRatio();

  // Update aspect ratio hack values
  // Won't take effect until next frame
  // Don't know if there is a better place for this code so there isn't a 1 frame delay
  if (g_ActiveConfig.bWidescreenHack)
  {
    float source_aspect = VideoInterface::GetAspectRatio();
    if (g_renderer && g_renderer->IsGameWidescreen())
      source_aspect = AspectToWidescreen(source_aspect);

    const float adjust = source_aspect / draw_aspect_ratio;
    if (adjust > 1)
    {
      // Vert+
      g_Config.fAspectRatioHackW = 1;
      g_Config.fAspectRatioHackH = 1 / adjust;
    }
    else
    {
      // Hor+
      g_Config.fAspectRatioHackW = adjust;
      g_Config.fAspectRatioHackH = 1;
    }
  }
  else
  {
    // Hack is disabled.
    g_Config.fAspectRatioHackW = 1;
    g_Config.fAspectRatioHackH = 1;
  }

  // The rendering window size
  const float win_width = static_cast<float>(m_backbuffer_width);
  const float win_height = static_cast<float>(m_backbuffer_height);

  // FIXME: this breaks at very low widget sizes
  // Make ControllerInterface aware of the render window region actually being used
  // to adjust mouse cursor inputs.
  g_controller_interface.SetAspectRatioAdjustment(draw_aspect_ratio / (win_width / win_height));

  float draw_width = draw_aspect_ratio;
  float draw_height = 1;

  // Crop the picture to a standard aspect ratio. (if enabled)
  auto [crop_width, crop_height] = ApplyStandardAspectCrop(draw_width, draw_height);

  // scale the picture to fit the rendering window
  if (win_width / win_height >= crop_width / crop_height)
  {
    // the window is flatter than the picture
    draw_width *= win_height / crop_height;
    crop_width *= win_height / crop_height;
    draw_height *= win_height / crop_height;
    crop_height = win_height;
  }
  else
  {
    // the window is skinnier than the picture
    draw_width *= win_width / crop_width;
    draw_height *= win_width / crop_width;
    crop_height *= win_width / crop_width;
    crop_width = win_width;
  }

  // ensure divisibility by 4 to make it compatible with all the video encoders
  draw_width = std::ceil(draw_width) - static_cast<int>(std::ceil(draw_width)) % 4;
  draw_height = std::ceil(draw_height) - static_cast<int>(std::ceil(draw_height)) % 4;

  m_target_rectangle.left = static_cast<int>(std::round(win_width / 2.0 - draw_width / 2.0));
  m_target_rectangle.top = static_cast<int>(std::round(win_height / 2.0 - draw_height / 2.0));
  m_target_rectangle.right = m_target_rectangle.left + static_cast<int>(draw_width);
  m_target_rectangle.bottom = m_target_rectangle.top + static_cast<int>(draw_height);
}

std::tuple<float, float> Presenter::ScaleToDisplayAspectRatio(const int width,
                                                              const int height) const
{
  // Scale either the width or height depending the content aspect ratio.
  // This way we preserve as much resolution as possible when scaling.
  float scaled_width = static_cast<float>(width);
  float scaled_height = static_cast<float>(height);
  const float draw_aspect = CalculateDrawAspectRatio();
  if (scaled_width / scaled_height >= draw_aspect)
    scaled_height = scaled_width / draw_aspect;
  else
    scaled_width = scaled_height * draw_aspect;
  return std::make_tuple(scaled_width, scaled_height);
}

std::tuple<int, int> Presenter::CalculateOutputDimensions(int width, int height) const
{
  width = std::max(width, 1);
  height = std::max(height, 1);

  auto [scaled_width, scaled_height] = ScaleToDisplayAspectRatio(width, height);

  // Apply crop if enabled.
  std::tie(scaled_width, scaled_height) = ApplyStandardAspectCrop(scaled_width, scaled_height);

  width = static_cast<int>(std::ceil(scaled_width));
  height = static_cast<int>(std::ceil(scaled_height));

  // UpdateDrawRectangle() makes sure that the rendered image is divisible by four for video
  // encoders, so do that here too to match it
  width -= width % 4;
  height -= height % 4;

  return std::make_tuple(width, height);
}

void Presenter::RenderXFBToScreen(const MathUtil::Rectangle<int>& target_rc,
                                  const AbstractTexture* source_texture,
                                  const MathUtil::Rectangle<int>& source_rc)
{
  if (!g_ActiveConfig.backend_info.bSupportsPostProcessing)
  {
    g_gfx->ShowImage(source_texture, source_rc);
    return;
  }

  if (g_ActiveConfig.stereo_mode == StereoMode::QuadBuffer &&
      g_ActiveConfig.backend_info.bUsesExplictQuadBuffering)
  {
    // Quad-buffered stereo is annoying on GL.
    g_gfx->SelectLeftBuffer();
    m_post_processor->BlitFromTexture(target_rc, source_rc, source_texture, 0);

    g_gfx->SelectRightBuffer();
    m_post_processor->BlitFromTexture(target_rc, source_rc, source_texture, 1);

    g_gfx->SelectMainBuffer();
  }
  else if (g_ActiveConfig.stereo_mode == StereoMode::SBS ||
           g_ActiveConfig.stereo_mode == StereoMode::TAB)
  {
    const auto [left_rc, right_rc] = ConvertStereoRectangle(target_rc);

    m_post_processor->BlitFromTexture(left_rc, source_rc, source_texture, 0);
    m_post_processor->BlitFromTexture(right_rc, source_rc, source_texture, 1);
  }
  else
  {
    m_post_processor->BlitFromTexture(target_rc, source_rc, source_texture, 0);
  }
}

bool Presenter::SubmitXFB(RcTcacheEntry xfb_entry, MathUtil::Rectangle<int>& xfb_rect, u64 ticks,
                          int frame_count)
{
  m_xfb_entry = std::move(xfb_entry);
  m_xfb_rect = xfb_rect;
  bool is_duplicate_frame = m_last_xfb_id == m_xfb_entry->id;

  if (!is_duplicate_frame || !g_ActiveConfig.bSkipPresentingDuplicateXFBs)
  {
    Present();

    if (g_frame_dumper->IsFrameDumping())
    {
      MathUtil::Rectangle<int> target_rect;
      if (!g_ActiveConfig.bInternalResolutionFrameDumps && !g_gfx->IsHeadless())
      {
        target_rect = GetTargetRectangle();
      }
      else
      {
        int width, height;
        std::tie(width, height) =
            CalculateOutputDimensions(m_xfb_rect.GetWidth(), m_xfb_rect.GetHeight());
        target_rect = MathUtil::Rectangle<int>(0, 0, width, height);
      }

      g_frame_dumper->DumpCurrentFrame(m_xfb_entry->texture.get(), m_xfb_rect, target_rect, ticks,
                                       frame_count);
    }
  }

  return is_duplicate_frame;
}

void Presenter::Present()
{
  m_last_xfb_id = m_xfb_entry->id;

  // Since we use the common pipelines here and draw vertices if a batch is currently being
  // built by the vertex loader, we end up trampling over its pointer, as we share the buffer
  // with the loader, and it has not been unmapped yet. Force a pipeline flush to avoid this.
  g_vertex_manager->Flush();

  // Render any UI elements to the draw list.
  m_onscreen_ui->Finalize();

  // Render the XFB to the screen.
  g_gfx->BeginUtilityDrawing();
  if (!g_gfx->IsHeadless())
  {
    g_gfx->BindBackbuffer({{0.0f, 0.0f, 0.0f, 1.0f}});

    UpdateDrawRectangle();

    // Adjust the source rectangle instead of using an oversized viewport to render the XFB.
    auto render_target_rc = GetTargetRectangle();
    auto render_source_rc = m_xfb_rect;
    AdjustRectanglesToFitBounds(&render_target_rc, &render_source_rc, m_backbuffer_width,
                                m_backbuffer_height);
    RenderXFBToScreen(render_target_rc, m_xfb_entry->texture.get(), render_source_rc);

    m_onscreen_ui->DrawImGui();

    // Present to the window system.
    {
      std::lock_guard<std::mutex> guard(m_swap_mutex);
      g_gfx->PresentBackbuffer();
    }

    // Update the window size based on the frame that was just rendered.
    // Due to depending on guest state, we need to call this every frame.
    SetWindowSize(m_xfb_rect.GetWidth(), m_xfb_rect.GetHeight());
  }

  m_onscreen_ui->BeginImGuiFrame(m_backbuffer_width, m_backbuffer_height);

  g_gfx->EndUtilityDrawing();
}

void Presenter::SetKeyMap(std::span<std::array<int, 2>> key_map)
{
  m_onscreen_ui->SetKeyMap(key_map);
}

void Presenter::SetKey(u32 key, bool is_down, const char* chars)
{
  m_onscreen_ui->SetKey(key, is_down, chars);
}

void Presenter::SetMousePos(float x, float y)
{
  m_onscreen_ui->SetMousePos(x, y);
}

void Presenter::SetMousePress(u32 button_mask)
{
  m_onscreen_ui->SetMousePress(button_mask);
}

}  // namespace VideoCommon