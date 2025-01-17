// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/OnScreenDisplay.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <string>

#include <fmt/format.h>
#include <imgui.h>

#include "UICommon/ImGuiMenu/ImGuiFrontend.h"

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Timer.h"

#include "Core/Config/MainSettings.h"
#include "Core/State.h"

#ifdef WINRT_XBOX
#include "DolphinWinRT/Host.h"
#include "DolphinWinRT/UWPUtils.h"
#endif

namespace OSD
{
constexpr float LEFT_MARGIN = 10.0f;         // Pixels to the left of OSD messages.
constexpr float TOP_MARGIN = 10.0f;          // Pixels above the first OSD message.
constexpr float WINDOW_PADDING = 4.0f;       // Pixels between subsequent OSD messages.
constexpr float MESSAGE_FADE_TIME = 1000.f;  // Ms to fade OSD messages at the end of their life.
constexpr float MESSAGE_DROP_TIME = 5000.f;  // Ms to drop OSD messages that has yet to ever render.

static std::atomic<int> s_obscured_pixels_left = 0;
static std::atomic<int> s_obscured_pixels_top = 0;
static ImGuiFrontend::UIState s_setting_state{};
static bool s_show_menu;

struct Message
{
  Message() = default;
  Message(std::string text_, u32 duration_, u32 color_)
      : text(std::move(text_)), duration(duration_), color(color_)
  {
    timer.Start();
  }
  s64 TimeRemaining() const { return duration - timer.ElapsedMs(); }
  std::string text;
  Common::Timer timer;
  u32 duration = 0;
  bool ever_drawn = false;
  u32 color = 0;
};
static std::multimap<MessageType, Message> s_messages;
static std::mutex s_messages_mutex;

static ImVec4 ARGBToImVec4(const u32 argb)
{
  return ImVec4(static_cast<float>((argb >> 16) & 0xFF) / 255.0f,
                static_cast<float>((argb >> 8) & 0xFF) / 255.0f,
                static_cast<float>((argb >> 0) & 0xFF) / 255.0f,
                static_cast<float>((argb >> 24) & 0xFF) / 255.0f);
}

static float DrawMessage(int index, Message& msg, const ImVec2& position, int time_left)
{
  // We have to provide a window name, and these shouldn't be duplicated.
  // So instead, we generate a name based on the number of messages drawn.
  const std::string window_name = fmt::format("osd_{}", index);

  // The size must be reset, otherwise the length of old messages could influence new ones.
  ImGui::SetNextWindowPos(position);
  ImGui::SetNextWindowSize(ImVec2(0.0f, 0.0f));

  // Gradually fade old messages away (except in their first frame)
  const float fade_time = std::max(std::min(MESSAGE_FADE_TIME, (float)msg.duration), 1.f);
  const float alpha = std::clamp(time_left / fade_time, 0.f, 1.f);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, msg.ever_drawn ? alpha : 1.0);

  float window_height = 0.0f;
  if (ImGui::Begin(window_name.c_str(), nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs |
                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav |
                       ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing))
  {
    // Use %s in case message contains %.
    ImGui::TextColored(ARGBToImVec4(msg.color), "%s", msg.text.c_str());
    window_height =
        ImGui::GetWindowSize().y + (WINDOW_PADDING * ImGui::GetIO().DisplayFramebufferScale.y);
  }

  ImGui::End();
  ImGui::PopStyleVar();

  msg.ever_drawn = true;

  return window_height;
}

void AddTypedMessage(MessageType type, std::string message, u32 ms, u32 argb)
{
  std::lock_guard lock{s_messages_mutex};
  s_messages.erase(type);
  s_messages.emplace(type, Message(std::move(message), ms, argb));
}

void AddMessage(std::string message, u32 ms, u32 argb)
{
  std::lock_guard lock{s_messages_mutex};
  s_messages.emplace(MessageType::Typeless, Message(std::move(message), ms, argb));
}

void DrawMessages()
{
  const bool draw_messages = Config::Get(Config::MAIN_OSD_MESSAGES);
  const float current_x =
      LEFT_MARGIN * ImGui::GetIO().DisplayFramebufferScale.x + s_obscured_pixels_left;
  float current_y = TOP_MARGIN * ImGui::GetIO().DisplayFramebufferScale.y + s_obscured_pixels_top;
  int index = 0;

  std::lock_guard lock{s_messages_mutex};

  for (auto it = s_messages.begin(); it != s_messages.end();)
  {
    Message& msg = it->second;
    const s64 time_left = msg.TimeRemaining();

    // Make sure we draw them at least once if they were printed with 0ms,
    // unless enough time has expired, in that case, we drop them
    if (time_left <= 0 && (msg.ever_drawn || -time_left >= MESSAGE_DROP_TIME))
    {
      it = s_messages.erase(it);
      continue;
    }
    else
    {
      ++it;
    }

    if (draw_messages)
      current_y += DrawMessage(index++, msg, ImVec2(current_x, current_y), time_left);
  }
}

void ClearMessages()
{
  std::lock_guard lock{s_messages_mutex};
  s_messages.clear();
}

void DrawInGameMenu()
{
  if (!s_show_menu)
    return;

  float frame_scale = ImGui::GetIO().DisplayFramebufferScale.x;
  ImGui::SetNextWindowSize(ImVec2(540 * frame_scale, 425 * frame_scale));
  ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x / 2 - (540 / 2) * frame_scale,
                                 ImGui::GetIO().DisplaySize.y / 2 - (425 / 2) * frame_scale));
  if (ImGui::Begin("Pause Menu", nullptr,
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoScrollbar))
  {
    if (ImGui::BeginTabBar("InGameTabs"))
    {
#ifdef WINRT_XBOX
      // todo - make the host handle this!!
      if (ImGui::BeginTabItem("General"))
      {
        if (ImGui::Button("Change Disc"))
        {
          UWP::PickDisc();
        }

        if (ImGui::Button("Exit Game"))
        {
          if (!UWP::g_tried_graceful_shutdown.TestAndClear())
          {
            UWP::g_shutdown_requested.Set();

            s_show_menu = false;
            Core::SetState(Core::State::Running);
          }
          else
          {
            exit(0);
          }
        }

        ImGui::EndTabItem();
      }
#endif

      if (ImGui::BeginTabItem("Save States"))
      {
        ImGui::TextWrapped("Warning: Savestates can be buggy with Dual Core enabled, do not rely on them "
                    "or you may risk losing progress.");
        for (int i = 0; i < 5; i++)
        {
          if (ImGui::BeginChild(std::format("savestate-{}", i).c_str(), ImVec2(-1, 75 * frame_scale), true))
          {
            ImGui::Text("Port %d - %s", i, State::GetInfoStringOfSlot(i).c_str());

            if (ImGui::Button(std::format("Load State in Port {}", i).c_str()))
            {
              Core::RunOnCPUThread([i] {
                s_show_menu = false;
                Core::SetState(Core::State::Running);
                State::Load(i);
              }, false);
            }

            if (ImGui::Button(std::format("Save State in Port {}", i).c_str()))
            {
              Core::RunOnCPUThread([i] {
                s_show_menu = false;
                Core::SetState(Core::State::Running);

                State::Save(i);
              }, false);
            }
          }

          ImGui::EndChild();
        }

        ImGui::EndTabItem();
      }

      if (ImGui::BeginTabItem("Options"))
      {
        ImGuiFrontend::DrawSettingsMenu(&s_setting_state, frame_scale);
        ImGui::EndTabItem();
      }

      if (ImGui::BeginTabItem("Netplay"))
      {
        if (g_netplay_client != nullptr)
        {
          ImGuiFrontend::DrawLobbyMenu();
        }
        else
        {
          ImGui::Text("You are not currently in any Netplay lobby.");
        }

        ImGui::EndTabItem();
      }

      ImGui::EndTabBar();
    }

    ImGui::End();
  }
}

void SetObscuredPixelsLeft(int width)
{
  s_obscured_pixels_left = width;
}

void SetObscuredPixelsTop(int height)
{
  s_obscured_pixels_top = height;
}

void ToggleShowSettings()
{
  s_show_menu = !s_show_menu;
}
}  // namespace OSD
