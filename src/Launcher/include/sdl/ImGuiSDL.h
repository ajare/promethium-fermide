#pragma once

#include <cstdint>
#include <cstring>

#include <SDL3/SDL.h>

#include "imgui/imgui.h"

struct ImGuiSdlData {
  SDL_Window* window;
  SDL_Cursor* mouseCursors[ImGuiMouseCursor_COUNT];
  SDL_Cursor* mouseLastCursor;
  char* clipboardTextData;
  uint64_t time;
  int mouseButtonsDown;
  int mouseLastLeaveFrame;

  ImGuiSdlData() {
    memset((void*)this, 0, sizeof(*this));
  }
};

void initialiseImGuiForSdl(SDL_Window* window);

void shutdownImGuiForSdl();

// Feed one polled SDL event to ImGui. The window owns the poll loop, so unlike
// the GLFW backend there is no per-kind callback to install - every event
// arrives here.
void imGuiProcessEvent(SDL_Event const& event);

void imGuiNewFrame();
