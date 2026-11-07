#include "sdl/ImGuiSDL.h"

static ImGuiSdlData* getBackendData() {
  return ImGui::GetCurrentContext() ? (ImGuiSdlData*)ImGui::GetIO().BackendPlatformUserData : nullptr;
}

static void setClipboardText(ImGuiContext*, char const* text) {
  SDL_SetClipboardText(text);
}

static char const* getClipboardText(ImGuiContext*) {
  auto bd = getBackendData();

  if (bd->clipboardTextData) {
    SDL_free(bd->clipboardTextData);
  }

  bd->clipboardTextData = SDL_GetClipboardText();
  return bd->clipboardTextData;
}

static void platformSetImeData(ImGuiContext*, ImGuiViewport*, ImGuiPlatformImeData* data) {
  if (data->WantVisible) {
    SDL_Rect r;
    r.x = (int)data->InputPos.x;
    r.y = (int)data->InputPos.y;
    r.w = 1;
    r.h = (int)data->InputLineHeight;

    // SDL3 scopes the IME area to a window and takes a cursor offset within it.
    SDL_SetTextInputArea(SDL_GetKeyboardFocus(), &r, 0);
  }
}

static ImGuiKey sdlKeyToImGuiKey(SDL_Keycode keycode, SDL_Scancode scancode) {
  switch (keycode) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_INSERT: return ImGuiKey_Insert;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_COMMA: return ImGuiKey_Comma;
    case SDLK_PERIOD: return ImGuiKey_Period;
    case SDLK_SEMICOLON: return ImGuiKey_Semicolon;
    case SDLK_CAPSLOCK: return ImGuiKey_CapsLock;
    case SDLK_SCROLLLOCK: return ImGuiKey_ScrollLock;
    case SDLK_NUMLOCKCLEAR: return ImGuiKey_NumLock;
    case SDLK_PRINTSCREEN: return ImGuiKey_PrintScreen;
    case SDLK_PAUSE: return ImGuiKey_Pause;
    case SDLK_KP_0: return ImGuiKey_Keypad0;
    case SDLK_KP_1: return ImGuiKey_Keypad1;
    case SDLK_KP_2: return ImGuiKey_Keypad2;
    case SDLK_KP_3: return ImGuiKey_Keypad3;
    case SDLK_KP_4: return ImGuiKey_Keypad4;
    case SDLK_KP_5: return ImGuiKey_Keypad5;
    case SDLK_KP_6: return ImGuiKey_Keypad6;
    case SDLK_KP_7: return ImGuiKey_Keypad7;
    case SDLK_KP_8: return ImGuiKey_Keypad8;
    case SDLK_KP_9: return ImGuiKey_Keypad9;
    case SDLK_KP_PERIOD: return ImGuiKey_KeypadDecimal;
    case SDLK_KP_DIVIDE: return ImGuiKey_KeypadDivide;
    case SDLK_KP_MULTIPLY: return ImGuiKey_KeypadMultiply;
    case SDLK_KP_MINUS: return ImGuiKey_KeypadSubtract;
    case SDLK_KP_PLUS: return ImGuiKey_KeypadAdd;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_KP_EQUALS: return ImGuiKey_KeypadEqual;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_LGUI: return ImGuiKey_LeftSuper;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    case SDLK_RGUI: return ImGuiKey_RightSuper;
    case SDLK_APPLICATION: return ImGuiKey_Menu;
    case SDLK_0: return ImGuiKey_0;
    case SDLK_1: return ImGuiKey_1;
    case SDLK_2: return ImGuiKey_2;
    case SDLK_3: return ImGuiKey_3;
    case SDLK_4: return ImGuiKey_4;
    case SDLK_5: return ImGuiKey_5;
    case SDLK_6: return ImGuiKey_6;
    case SDLK_7: return ImGuiKey_7;
    case SDLK_8: return ImGuiKey_8;
    case SDLK_9: return ImGuiKey_9;
    case SDLK_A: return ImGuiKey_A;
    case SDLK_B: return ImGuiKey_B;
    case SDLK_C: return ImGuiKey_C;
    case SDLK_D: return ImGuiKey_D;
    case SDLK_E: return ImGuiKey_E;
    case SDLK_F: return ImGuiKey_F;
    case SDLK_G: return ImGuiKey_G;
    case SDLK_H: return ImGuiKey_H;
    case SDLK_I: return ImGuiKey_I;
    case SDLK_J: return ImGuiKey_J;
    case SDLK_K: return ImGuiKey_K;
    case SDLK_L: return ImGuiKey_L;
    case SDLK_M: return ImGuiKey_M;
    case SDLK_N: return ImGuiKey_N;
    case SDLK_O: return ImGuiKey_O;
    case SDLK_P: return ImGuiKey_P;
    case SDLK_Q: return ImGuiKey_Q;
    case SDLK_R: return ImGuiKey_R;
    case SDLK_S: return ImGuiKey_S;
    case SDLK_T: return ImGuiKey_T;
    case SDLK_U: return ImGuiKey_U;
    case SDLK_V: return ImGuiKey_V;
    case SDLK_W: return ImGuiKey_W;
    case SDLK_X: return ImGuiKey_X;
    case SDLK_Y: return ImGuiKey_Y;
    case SDLK_Z: return ImGuiKey_Z;
    case SDLK_F1: return ImGuiKey_F1;
    case SDLK_F2: return ImGuiKey_F2;
    case SDLK_F3: return ImGuiKey_F3;
    case SDLK_F4: return ImGuiKey_F4;
    case SDLK_F5: return ImGuiKey_F5;
    case SDLK_F6: return ImGuiKey_F6;
    case SDLK_F7: return ImGuiKey_F7;
    case SDLK_F8: return ImGuiKey_F8;
    case SDLK_F9: return ImGuiKey_F9;
    case SDLK_F10: return ImGuiKey_F10;
    case SDLK_F11: return ImGuiKey_F11;
    case SDLK_F12: return ImGuiKey_F12;
    case SDLK_F13: return ImGuiKey_F13;
    case SDLK_F14: return ImGuiKey_F14;
    case SDLK_F15: return ImGuiKey_F15;
    case SDLK_F16: return ImGuiKey_F16;
    case SDLK_F17: return ImGuiKey_F17;
    case SDLK_F18: return ImGuiKey_F18;
    case SDLK_F19: return ImGuiKey_F19;
    case SDLK_F20: return ImGuiKey_F20;
    case SDLK_F21: return ImGuiKey_F21;
    case SDLK_F22: return ImGuiKey_F22;
    case SDLK_F23: return ImGuiKey_F23;
    case SDLK_F24: return ImGuiKey_F24;
    case SDLK_AC_BACK: return ImGuiKey_AppBack;
    case SDLK_AC_FORWARD: return ImGuiKey_AppForward;
    default: break;
  }

  // Keycodes for the punctuation keys change with the keyboard layout, so the
  // ones ImGui indexes positionally are resolved from the scancode instead.
  switch (scancode) {
    case SDL_SCANCODE_GRAVE: return ImGuiKey_GraveAccent;
    case SDL_SCANCODE_MINUS: return ImGuiKey_Minus;
    case SDL_SCANCODE_EQUALS: return ImGuiKey_Equal;
    case SDL_SCANCODE_LEFTBRACKET: return ImGuiKey_LeftBracket;
    case SDL_SCANCODE_RIGHTBRACKET: return ImGuiKey_RightBracket;
    case SDL_SCANCODE_NONUSBACKSLASH: return ImGuiKey_Oem102;
    case SDL_SCANCODE_BACKSLASH: return ImGuiKey_Backslash;
    case SDL_SCANCODE_SEMICOLON: return ImGuiKey_Semicolon;
    case SDL_SCANCODE_APOSTROPHE: return ImGuiKey_Apostrophe;
    case SDL_SCANCODE_COMMA: return ImGuiKey_Comma;
    case SDL_SCANCODE_PERIOD: return ImGuiKey_Period;
    case SDL_SCANCODE_SLASH: return ImGuiKey_Slash;
    default: break;
  }

  return ImGuiKey_None;
}

static int sdlButtonToImGuiButton(uint8_t button) {
  switch (button) {
    case SDL_BUTTON_LEFT: return 0;
    case SDL_BUTTON_RIGHT: return 1;
    case SDL_BUTTON_MIDDLE: return 2;
    case SDL_BUTTON_X1: return 3;
    case SDL_BUTTON_X2: return 4;
    default: return -1;
  }
}

static void updateKeyModifiers(SDL_Keymod mods) {
  ImGuiIO& io = ImGui::GetIO();

  io.AddKeyEvent(ImGuiMod_Ctrl, (mods & SDL_KMOD_CTRL) != 0);
  io.AddKeyEvent(ImGuiMod_Shift, (mods & SDL_KMOD_SHIFT) != 0);
  io.AddKeyEvent(ImGuiMod_Alt, (mods & SDL_KMOD_ALT) != 0);
  io.AddKeyEvent(ImGuiMod_Super, (mods & SDL_KMOD_GUI) != 0);
}

void initialiseImGuiForSdl(SDL_Window* window) {
  ImGuiIO& io = ImGui::GetIO();

  auto bd = new ImGuiSdlData();

  io.BackendPlatformUserData = (void*)bd;
  io.BackendPlatformName = "SDL3";
  io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
  io.BackendFlags |= ImGuiBackendFlags_HasSetMousePos;

  bd->window = window;

  bd->mouseCursors[ImGuiMouseCursor_Arrow] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
  bd->mouseCursors[ImGuiMouseCursor_TextInput] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
  bd->mouseCursors[ImGuiMouseCursor_ResizeAll] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_MOVE);
  bd->mouseCursors[ImGuiMouseCursor_ResizeNS] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NS_RESIZE);
  bd->mouseCursors[ImGuiMouseCursor_ResizeEW] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_EW_RESIZE);
  bd->mouseCursors[ImGuiMouseCursor_ResizeNESW] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NESW_RESIZE);
  bd->mouseCursors[ImGuiMouseCursor_ResizeNWSE] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NWSE_RESIZE);
  bd->mouseCursors[ImGuiMouseCursor_Hand] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
  bd->mouseCursors[ImGuiMouseCursor_Wait] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_WAIT);
  bd->mouseCursors[ImGuiMouseCursor_Progress] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_PROGRESS);
  bd->mouseCursors[ImGuiMouseCursor_NotAllowed] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NOT_ALLOWED);

  ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();

  platformIO.Platform_SetClipboardTextFn = setClipboardText;
  platformIO.Platform_GetClipboardTextFn = getClipboardText;
  platformIO.Platform_ClipboardUserData = nullptr;
  platformIO.Platform_SetImeDataFn = platformSetImeData;

  // Set platform dependent data in viewport. SDL3 owns the native window, so
  // unlike the GLFW backend there is no WndProc to hook for mouse source data -
  // SDL reports that on the event itself.
  ImGuiViewport* mainViewport = ImGui::GetMainViewport();
  mainViewport->PlatformHandle = (void*)window;
#ifdef _WIN32
  mainViewport->PlatformHandleRaw = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#endif
}

void shutdownImGuiForSdl() {
  auto bd = getBackendData();

  if (!bd) {
    return;
  }

  ImGuiIO& io = ImGui::GetIO();

  if (bd->clipboardTextData) {
    SDL_free(bd->clipboardTextData);
  }

  for (ImGuiMouseCursor cursor = 0; cursor < ImGuiMouseCursor_COUNT; cursor++) {
    SDL_DestroyCursor(bd->mouseCursors[cursor]);
  }

  io.BackendPlatformName = nullptr;
  io.BackendPlatformUserData = nullptr;
  io.BackendFlags &= ~(ImGuiBackendFlags_HasMouseCursors | ImGuiBackendFlags_HasSetMousePos | ImGuiBackendFlags_HasGamepad);

  delete bd;
}

void imGuiProcessEvent(SDL_Event const& event) {
  auto bd = getBackendData();

  if (!bd) {
    return;
  }

  ImGuiIO& io = ImGui::GetIO();

  switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
      updateKeyModifiers((SDL_Keymod)event.key.mod);

      ImGuiKey key = sdlKeyToImGuiKey(event.key.key, event.key.scancode);
      if (key != ImGuiKey_None) {
        io.AddKeyEvent(key, event.type == SDL_EVENT_KEY_DOWN);
        // The native/legacy key index must be a scancode, not the SDLK_* value
        // (arrow keys are far outside that index range).
        io.SetKeyEventNativeData(key, event.key.key, event.key.scancode, event.key.scancode);
      }
      break;
    }

    case SDL_EVENT_TEXT_INPUT:
      io.AddInputCharactersUTF8(event.text.text);
      break;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      int button = sdlButtonToImGuiButton(event.button.button);
      if (button == -1) {
        break;
      }

      bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;

      io.AddMouseSourceEvent(event.button.which == SDL_TOUCH_MOUSEID ? ImGuiMouseSource_TouchScreen : ImGuiMouseSource_Mouse);
      io.AddMouseButtonEvent(button, down);

      if (down) {
        bd->mouseButtonsDown |= (1 << button);
      } else {
        bd->mouseButtonsDown &= ~(1 << button);
      }
      break;
    }

    case SDL_EVENT_MOUSE_WHEEL:
      io.AddMouseSourceEvent(event.wheel.which == SDL_TOUCH_MOUSEID ? ImGuiMouseSource_TouchScreen : ImGuiMouseSource_Mouse);
      io.AddMouseWheelEvent(-event.wheel.x, event.wheel.y);
      break;

    case SDL_EVENT_MOUSE_MOTION:
      io.AddMouseSourceEvent(event.motion.which == SDL_TOUCH_MOUSEID ? ImGuiMouseSource_TouchScreen : ImGuiMouseSource_Mouse);
      io.AddMousePosEvent(event.motion.x, event.motion.y);
      break;

    case SDL_EVENT_WINDOW_MOUSE_ENTER:
      bd->mouseLastLeaveFrame = 0;
      break;

    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
      // Defer the "mouse is gone" position by a frame: a leave that arrives
      // mid-drag would otherwise cancel the drag.
      bd->mouseLastLeaveFrame = ImGui::GetFrameCount() + 1;
      break;

    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      io.AddFocusEvent(true);
      break;

    case SDL_EVENT_WINDOW_FOCUS_LOST:
      io.AddFocusEvent(false);
      break;
  }
}

static void updateMouseData() {
  auto bd = getBackendData();
  ImGuiIO& io = ImGui::GetIO();

  bool isWindowFocused = (SDL_GetWindowFlags(bd->window) & SDL_WINDOW_INPUT_FOCUS) != 0;

  if (isWindowFocused) {
    // (Optional) Set OS mouse position from Dear ImGui if requested (rarely
    // used, only when io.ConfigNavMoveSetMousePos is enabled by user)
    if (io.WantSetMousePos) {
      SDL_WarpMouseInWindow(bd->window, io.MousePos.x, io.MousePos.y);
    }
  }
}

static void updateMouseCursor() {
  auto bd = getBackendData();
  ImGuiIO& io = ImGui::GetIO();

  // Relative mouse mode is the game's own mouse-look capture - leave the cursor
  // alone while it is on, the same way the GLFW backend skipped this whenever
  // the cursor was disabled.
  if ((io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) || SDL_GetWindowRelativeMouseMode(bd->window)) {
    return;
  }

  ImGuiMouseCursor imguiCursor = ImGui::GetMouseCursor();

  if (imguiCursor == ImGuiMouseCursor_None || io.MouseDrawCursor) {
    // Hide OS mouse cursor if imgui is drawing it or if it wants no cursor
    SDL_HideCursor();
  } else {
    auto expectedCursor = bd->mouseCursors[imguiCursor] ? bd->mouseCursors[imguiCursor] : bd->mouseCursors[ImGuiMouseCursor_Arrow];

    if (bd->mouseLastCursor != expectedCursor) {
      SDL_SetCursor(expectedCursor);
      bd->mouseLastCursor = expectedCursor;
    }

    SDL_ShowCursor();
  }
}

void imGuiNewFrame() {
  auto bd = getBackendData();
  ImGuiIO& io = ImGui::GetIO();

  // Setup display size (every frame to accommodate for window resizing)
  int w, h;
  int displayWidth, displayHeight;

  SDL_GetWindowSize(bd->window, &w, &h);

  if (SDL_GetWindowFlags(bd->window) & SDL_WINDOW_MINIMIZED) {
    w = h = 0;
  }

  SDL_GetWindowSizeInPixels(bd->window, &displayWidth, &displayHeight);

  io.DisplaySize = ImVec2((float)w, (float)h);
  if (w > 0 && h > 0) {
    io.DisplayFramebufferScale = ImVec2((float)displayWidth / (float)w, (float)displayHeight / (float)h);
  }

  // Setup time step. SDL_GetTicks() is only millisecond resolution, so use the
  // performance counter, and accept that it is not strictly monotonic on VMs.
  static uint64_t frequency = SDL_GetPerformanceFrequency();
  uint64_t currentTime = SDL_GetPerformanceCounter();
  if (currentTime <= bd->time) {
    currentTime = bd->time + 1;
  }

  io.DeltaTime = bd->time > 0 ? (float)((double)(currentTime - bd->time) / frequency) : (float)(1.0f / 60.0f);
  bd->time = currentTime;

  if (bd->mouseLastLeaveFrame && bd->mouseLastLeaveFrame >= ImGui::GetFrameCount() && bd->mouseButtonsDown == 0) {
    bd->mouseLastLeaveFrame = 0;
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
  }

  updateMouseData();
  updateMouseCursor();
}
