#include <stdexcept>
#include <format>

#include <willpower/application/Key.h>
#include <willpower/application/MouseButton.h>

#include "sdl/WindowSDL.h"
#include "sdl/ImGuiSDL.h"

using namespace std;
using namespace wp;

extern Logger* gLogger;

extern bool gDisplayDebugEnabled;

namespace {
bool translateMouseButton(uint8_t button, application::MouseButton& mouseButton) {
  switch (button) {
    case SDL_BUTTON_LEFT:
      mouseButton = application::MouseButton::Left;
      return true;
    case SDL_BUTTON_RIGHT:
      mouseButton = application::MouseButton::Right;
      return true;
    case SDL_BUTTON_MIDDLE:
      mouseButton = application::MouseButton::Middle;
      return true;
    default:
      return false;
  }
}
}  // namespace

WindowSDL::WindowSDL(string const& title, ProgramOptions const& options)
    : Window(title, options), mWindow(nullptr), mContextGL(nullptr), mContentScale(1.0f), mActive(true), mStateMgr(nullptr), mVirtualMouseX(0.0f), mVirtualMouseY(0.0f), mCursorShown(true), mWarpMouseCapture(false), mIgnoreWarpMotion(false) {
  mKeyTranslator[SDLK_ESCAPE] = application::Key::Escape;
  mKeyTranslator[SDLK_1] = application::Key::_1;
  mKeyTranslator[SDLK_2] = application::Key::_2;
  mKeyTranslator[SDLK_3] = application::Key::_3;
  mKeyTranslator[SDLK_4] = application::Key::_4;
  mKeyTranslator[SDLK_5] = application::Key::_5;
  mKeyTranslator[SDLK_6] = application::Key::_6;
  mKeyTranslator[SDLK_7] = application::Key::_7;
  mKeyTranslator[SDLK_8] = application::Key::_8;
  mKeyTranslator[SDLK_9] = application::Key::_9;
  mKeyTranslator[SDLK_0] = application::Key::_0;
  mKeyTranslator[SDLK_MINUS] = application::Key::Minus;
  mKeyTranslator[SDLK_EQUALS] = application::Key::Equals;
  mKeyTranslator[SDLK_BACKSPACE] = application::Key::Backspace;
  mKeyTranslator[SDLK_TAB] = application::Key::Tab;
  mKeyTranslator[SDLK_A] = application::Key::A;
  mKeyTranslator[SDLK_B] = application::Key::B;
  mKeyTranslator[SDLK_C] = application::Key::C;
  mKeyTranslator[SDLK_D] = application::Key::D;
  mKeyTranslator[SDLK_E] = application::Key::E;
  mKeyTranslator[SDLK_F] = application::Key::F;
  mKeyTranslator[SDLK_G] = application::Key::G;
  mKeyTranslator[SDLK_H] = application::Key::H;
  mKeyTranslator[SDLK_I] = application::Key::I;
  mKeyTranslator[SDLK_J] = application::Key::J;
  mKeyTranslator[SDLK_K] = application::Key::K;
  mKeyTranslator[SDLK_L] = application::Key::L;
  mKeyTranslator[SDLK_M] = application::Key::M;
  mKeyTranslator[SDLK_N] = application::Key::N;
  mKeyTranslator[SDLK_O] = application::Key::O;
  mKeyTranslator[SDLK_P] = application::Key::P;
  mKeyTranslator[SDLK_Q] = application::Key::Q;
  mKeyTranslator[SDLK_R] = application::Key::R;
  mKeyTranslator[SDLK_S] = application::Key::S;
  mKeyTranslator[SDLK_T] = application::Key::T;
  mKeyTranslator[SDLK_U] = application::Key::U;
  mKeyTranslator[SDLK_V] = application::Key::V;
  mKeyTranslator[SDLK_W] = application::Key::W;
  mKeyTranslator[SDLK_X] = application::Key::X;
  mKeyTranslator[SDLK_Y] = application::Key::Y;
  mKeyTranslator[SDLK_Z] = application::Key::Z;

  mKeyTranslator[SDLK_LEFTBRACKET] = application::Key::LeftBracket;
  mKeyTranslator[SDLK_RIGHTBRACKET] = application::Key::RightBracket;
  mKeyTranslator[SDLK_RETURN] = application::Key::Enter;
  mKeyTranslator[SDLK_LCTRL] = application::Key::LeftControl;
  mKeyTranslator[SDLK_RCTRL] = application::Key::RightControl;
  mKeyTranslator[SDLK_SEMICOLON] = application::Key::Semicolon;
  mKeyTranslator[SDLK_AT] = application::Key::Apostrophe;
  mKeyTranslator[SDLK_GRAVE] = application::Key::Tilde;
  mKeyTranslator[SDLK_LSHIFT] = application::Key::LeftShift;
  mKeyTranslator[SDLK_RSHIFT] = application::Key::RightShift;
  mKeyTranslator[SDLK_BACKSLASH] = application::Key::Backslash;
  mKeyTranslator[SDLK_COMMA] = application::Key::Comma;
  mKeyTranslator[SDLK_PERIOD] = application::Key::Period;
  mKeyTranslator[SDLK_SLASH] = application::Key::Slash;
  mKeyTranslator[SDLK_HASH] = application::Key::Hash;
  mKeyTranslator[SDLK_LALT] = application::Key::LeftAlt;
  mKeyTranslator[SDLK_RALT] = application::Key::RightAlt;
  mKeyTranslator[SDLK_SPACE] = application::Key::Space;
  mKeyTranslator[SDLK_CAPSLOCK] = application::Key::CapsLock;
  mKeyTranslator[SDLK_NUMLOCKCLEAR] = application::Key::NumLock;
  mKeyTranslator[SDLK_SCROLLLOCK] = application::Key::ScrollLock;
  mKeyTranslator[SDLK_F1] = application::Key::F1;
  mKeyTranslator[SDLK_F2] = application::Key::F2;
  mKeyTranslator[SDLK_F3] = application::Key::F3;
  mKeyTranslator[SDLK_F4] = application::Key::F4;
  mKeyTranslator[SDLK_F5] = application::Key::F5;
  mKeyTranslator[SDLK_F6] = application::Key::F6;
  mKeyTranslator[SDLK_F7] = application::Key::F7;
  mKeyTranslator[SDLK_F8] = application::Key::F8;
  mKeyTranslator[SDLK_F9] = application::Key::F9;
  mKeyTranslator[SDLK_F10] = application::Key::F10;
  mKeyTranslator[SDLK_F11] = application::Key::F11;
  mKeyTranslator[SDLK_F12] = application::Key::F12;
  mKeyTranslator[SDLK_KP_MULTIPLY] = application::Key::NumpadMultiply;
  mKeyTranslator[SDLK_KP_MINUS] = application::Key::NumpadMinus;
  mKeyTranslator[SDLK_KP_PLUS] = application::Key::NumpadPlus;
  mKeyTranslator[SDLK_KP_DIVIDE] = application::Key::NumpadDivide;
  mKeyTranslator[SDLK_KP_0] = application::Key::Numpad_0;
  mKeyTranslator[SDLK_KP_1] = application::Key::Numpad_1;
  mKeyTranslator[SDLK_KP_2] = application::Key::Numpad_2;
  mKeyTranslator[SDLK_KP_3] = application::Key::Numpad_3;
  mKeyTranslator[SDLK_KP_4] = application::Key::Numpad_4;
  mKeyTranslator[SDLK_KP_5] = application::Key::Numpad_5;
  mKeyTranslator[SDLK_KP_6] = application::Key::Numpad_6;
  mKeyTranslator[SDLK_KP_7] = application::Key::Numpad_7;
  mKeyTranslator[SDLK_KP_8] = application::Key::Numpad_8;
  mKeyTranslator[SDLK_KP_9] = application::Key::Numpad_9;
  mKeyTranslator[SDLK_KP_PERIOD] = application::Key::NumpadPeriod;
  mKeyTranslator[SDLK_KP_ENTER] = application::Key::NumpadEnter;
  mKeyTranslator[SDLK_PRINTSCREEN] = application::Key::PrintScreen;
  mKeyTranslator[SDLK_PAUSE] = application::Key::Pause;
  mKeyTranslator[SDLK_HOME] = application::Key::Home;
  mKeyTranslator[SDLK_END] = application::Key::End;
  mKeyTranslator[SDLK_UP] = application::Key::UpArrow;
  mKeyTranslator[SDLK_DOWN] = application::Key::DownArrow;
  mKeyTranslator[SDLK_LEFT] = application::Key::LeftArrow;
  mKeyTranslator[SDLK_RIGHT] = application::Key::RightArrow;
  mKeyTranslator[SDLK_PAGEUP] = application::Key::PageUp;
  mKeyTranslator[SDLK_PAGEDOWN] = application::Key::PageDown;
  mKeyTranslator[SDLK_INSERT] = application::Key::Insert;
  mKeyTranslator[SDLK_DELETE] = application::Key::Delete;
}

WindowSDL::~WindowSDL() {
  destroy();
}

SDL_Window* WindowSDL::getWindow() {
  return mWindow;
}

float WindowSDL::getContentScale() const {
  return mContentScale;
}

bool WindowSDL::isActive() const {
  return mActive;
}

void WindowSDL::create() {
  gLogger->info(format("Creating window at {}x{}", mWidth, mHeight));

  // Use an OpenGL 3.2 core context. MassivePolyPusher's point-based text path
  // relies on core point-sprite behaviour; a compatibility context requires
  // GL_POINT_SPRITE to be enabled and otherwise produces transparent glyphs.
  if (!SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE)) {
    string err = SDL_GetError();
    throw runtime_error(("Could not set OpenGL attribute: " + err).c_str());
  }

  if (!SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3)) {
    string err = SDL_GetError();
    throw runtime_error(("Could not set OpenGL attribute: " + err).c_str());
  }

  if (!SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2)) {
    string err = SDL_GetError();
    throw runtime_error(("Could not set OpenGL attribute: " + err).c_str());
  }

  if (!SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1)) {
    string err = SDL_GetError();
    throw runtime_error(("Could not set OpenGL attribute: " + err).c_str());
  }

  if (!SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24)) {
    string err = SDL_GetError();
    throw runtime_error(("Could not set OpenGL attribute: " + err).c_str());
  }

  // Relative-mouse hints are read when the mode is enabled, so set them first.
  // Motion is left at the scale SDL reports it: the application's own mouse
  // sensitivity is the one place turn speed is scaled.
  if (!SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_MODE_CENTER, "1")) {
    throw runtime_error("Could not set hint: SDL_HINT_MOUSE_RELATIVE_MODE_CENTER");
  }

  // Create window
  SDL_WindowFlags windowFlags = SDL_WINDOW_OPENGL;

  if (mFullscreen) {
    windowFlags |= SDL_WINDOW_FULLSCREEN;
  }

  mWindow = SDL_CreateWindow(mTitle.c_str(), mWidth, mHeight, windowFlags);

  if (!mWindow) {
    string err = SDL_GetError();
    throw runtime_error(("Could not create SDL window: " + err).c_str());
  }

  // An SDL3 fullscreen window is borderless-desktop unless it is given an
  // explicit mode. Pin the closest mode to the requested size so fullscreen
  // changes video mode the way GLFW's monitor argument did.
  SDL_DisplayMode fullscreenMode;
  if (SDL_GetClosestFullscreenDisplayMode(SDL_GetDisplayForWindow(mWindow), mWidth, mHeight, 0.0f, false, &fullscreenMode)) {
    SDL_SetWindowFullscreenMode(mWindow, &fullscreenMode);
  }

#ifdef _DEBUG
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
#endif

  mContextGL = SDL_GL_CreateContext(mWindow);
  if (!SDL_GL_MakeCurrent(mWindow, mContextGL)) {
    throw runtime_error("Could not set the OpenGL context.");
  }

  // Log what the driver actually handed back rather than what was asked for -
  // a pixel format without depth bits silently turns every depth test into a
  // pass, which looks like depth testing having been switched off.
  int depthBits = 0, doubleBuffered = 0;
  SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &depthBits);
  SDL_GL_GetAttribute(SDL_GL_DOUBLEBUFFER, &doubleBuffered);
  gLogger->info(format("GL context: depth bits {}, double buffered {}", depthBits, doubleBuffered));

  // Get content scale for 2d rendering
  mContentScale = SDL_GetWindowDisplayScale(mWindow);
  if (mContentScale <= 0.0f) {
    mContentScale = 1.0f;
  }
  gLogger->info(format("Setting content scale to {}", mContentScale));

  // This window presents through OpenGL, so vsync is the GL swap interval.
  gLogger->info(format("Setting Vsync to {}", mVSync));
  SDL_GL_SetSwapInterval(mVSync ? 1 : 0);

  // SDL3 leaves text input disabled until it is asked for, and ImGui's text
  // fields need the resulting SDL_EVENT_TEXT_INPUT.
  SDL_StartTextInput(mWindow);

  // Capture the cursor for mouse-look, matching GLFW_CURSOR_DISABLED.
  showCursor(false);

  mVirtualMouseX = (float)mWidth / 2.0f;
  mVirtualMouseY = (float)mHeight / 2.0f;

  mActive = true;
}

void WindowSDL::destroy() {
  if (mContextGL) {
    SDL_GL_DestroyContext(mContextGL);
    mContextGL = nullptr;
  }
  if (mWindow) {
    if (gLogger) {
      gLogger->info("Destroying window");
    }
    SDL_DestroyWindow(mWindow);
    mWindow = nullptr;
  }
}

void WindowSDL::setFullscreen(bool fullscreen) {
  if (fullscreen) {
    SDL_SetWindowFullscreen(mWindow, SDL_WINDOW_FULLSCREEN);
  } else {
    SDL_SetWindowFullscreen(mWindow, 0);
  }

  mFullscreen = fullscreen;
}

void WindowSDL::setSize(int width, int height) {
  mWidth = width;
  mHeight = height;

  // if we're fullscreen, then go to windowed first, to stop other windows in background being resized.
  if (mFullscreen) {
    setFullscreen(false);
    SDL_SetWindowSize(mWindow, width, height);
    setFullscreen(true);
  } else {
    SDL_SetWindowSize(mWindow, width, height);
  }
}

void WindowSDL::show() {
  SDL_GL_SwapWindow(mWindow);
}

void WindowSDL::warpCapturedMouseToCenter() {
  mIgnoreWarpMotion = true;
  SDL_WarpMouseInWindow(
      mWindow, static_cast<float>(mWidth) / 2.0f,
      static_cast<float>(mHeight) / 2.0f);
}

void WindowSDL::captureMouse() {
  // Relative mode supplies unbounded motion, while an explicit grab ensures
  // the platform confines the pointer to this window. SDL documents relative
  // mode as doing both, but its X11 backend can enable relative events without
  // establishing confinement.
  auto const relativeModeEnabled =
      SDL_SetWindowRelativeMouseMode(mWindow, true);
  auto const relativeError =
      relativeModeEnabled ? string{} : string(SDL_GetError());

  if (!SDL_SetWindowMouseGrab(mWindow, true)) {
    throw runtime_error(format(
        "Could not capture mouse (relative mode: {}; grab: {})",
        relativeModeEnabled ? "enabled" : relativeError, SDL_GetError()));
  }

  if (relativeModeEnabled) {
    mWarpMouseCapture = false;
    return;
  }

  // Some Linux X11 installations lack XInput2 and reject relative mode;
  // centre warping under the grab preserves infinite mouse-look there.
  gLogger->warn(format(
      "Relative mouse mode unavailable ({}); using centre-warp fallback",
      relativeError));
  mWarpMouseCapture = true;
  warpCapturedMouseToCenter();
}

void WindowSDL::showCursor(bool show) {
  if (!mWindow || show == mCursorShown) {
    return;
  }

  if (show) {
    if (!SDL_SetWindowRelativeMouseMode(mWindow, false)) {
      gLogger->warn(
          format("Could not disable relative mouse mode: {}", SDL_GetError()));
    }
    if (!SDL_SetWindowMouseGrab(mWindow, false)) {
      gLogger->warn(
          format("Could not release mouse grab: {}", SDL_GetError()));
    }
    mWarpMouseCapture = false;
    mIgnoreWarpMotion = false;
    SDL_ShowCursor();
  } else {
    captureMouse();
    SDL_HideCursor();
  }

  mCursorShown = show;
}

void WindowSDL::setStateManager(StateManager* mgr) {
  mStateMgr = mgr;
}

application::KeyModifiers WindowSDL::getKeyModifiers(uint16_t mod) {
  // Only the individual left/right bits are set, and with OR rather than +.
  // KeyModifiers::Shift/Ctrl/Alt are the *union* of their two side bits
  // (Shift == LeftShift | RightShift), so setting them as well would carry into
  // an unrelated modifier - a lone left shift came out as LeftCtrl.
  uint32_t km = (int)application::KeyModifiers::None;

  if (mod & SDL_KMOD_LSHIFT)
    km |= (int)application::KeyModifiers::LeftShift;
  if (mod & SDL_KMOD_RSHIFT)
    km |= (int)application::KeyModifiers::RightShift;
  if (mod & SDL_KMOD_LCTRL)
    km |= (int)application::KeyModifiers::LeftCtrl;
  if (mod & SDL_KMOD_RCTRL)
    km |= (int)application::KeyModifiers::RightCtrl;
  if (mod & SDL_KMOD_LALT)
    km |= (int)application::KeyModifiers::LeftAlt;
  if (mod & SDL_KMOD_RALT)
    km |= (int)application::KeyModifiers::RightAlt;
  if (mod & SDL_KMOD_NUM)
    km |= (int)application::KeyModifiers::NumLock;
  if (mod & SDL_KMOD_CAPS)
    km |= (int)application::KeyModifiers::CapsLock;

  return (application::KeyModifiers)km;
}

bool WindowSDL::translateKey(SDL_Keycode keycode, wp::application::Key& key) const {
  auto it = mKeyTranslator.find(keycode);

  if (it == mKeyTranslator.end()) {
    return false;
  }

  key = it->second;
  return true;
}

void WindowSDL::processEvents(StateManager* stateMgr) {
  // Copy current keys to old
  saveKeys();

  bool imGuiActive = stateMgr && stateMgr->imGuiActive();

  SDL_Event evt;
  while (SDL_PollEvent(&evt)) {
    if (imGuiActive) {
      imGuiProcessEvent(evt);
    }

    wp::application::Key key;
    switch (evt.type) {
      case SDL_EVENT_KEY_DOWN:
        // SDL repeats a held key; only the first press is an edge.
        if (evt.key.repeat) {
          break;
        }

        if (translateKey(evt.key.key, key)) {
          setKey(key, true);

          if (keyPressed(key)) {
            stateMgr->injectKeyInput(application::KeyEvent::Pressed, key, getKeyModifiers(evt.key.mod));
          }

          // Debugging
          if (key == application::Key::F1) {
            gDisplayDebugEnabled = !gDisplayDebugEnabled;
          }
        }
        break;

      case SDL_EVENT_KEY_UP:
        if (translateKey(evt.key.key, key)) {
          setKey(key, false);

          if (keyReleased(key)) {
            stateMgr->injectKeyInput(application::KeyEvent::Released, key, getKeyModifiers(evt.key.mod));
          }
        }
        break;

      case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (application::MouseButton mouseButton; translateMouseButton(evt.button.button, mouseButton)) {
          stateMgr->injectMouseButtonInput(application::MouseButtonEvent::Pressed, mouseButton, getKeyModifiers(SDL_GetModState()));
        }
        break;

      case SDL_EVENT_MOUSE_BUTTON_UP:
        if (application::MouseButton mouseButton; translateMouseButton(evt.button.button, mouseButton)) {
          stateMgr->injectMouseButtonInput(application::MouseButtonEvent::Released, mouseButton, getKeyModifiers(SDL_GetModState()));
        }
        break;

      case SDL_EVENT_MOUSE_WHEEL:
        stateMgr->injectMouseWheelInput((int)evt.wheel.y);
        break;

      case SDL_EVENT_MOUSE_MOTION:
        // The game turns this position into a per-frame delta, so captured
        // input accumulates an unbounded virtual position. Native relative
        // mode reports xrel/yrel directly. The fallback ignores the synthetic
        // centre-warp event but retains every physical relative movement.
        if (SDL_GetWindowRelativeMouseMode(mWindow)) {
          mVirtualMouseX += evt.motion.xrel;
          mVirtualMouseY += evt.motion.yrel;
        } else if (mWarpMouseCapture) {
          auto const centerX = static_cast<float>(mWidth) / 2.0f;
          auto const centerY = static_cast<float>(mHeight) / 2.0f;
          if (mIgnoreWarpMotion && evt.motion.x == centerX &&
              evt.motion.y == centerY) {
            mIgnoreWarpMotion = false;
            break;
          }
          mVirtualMouseX += evt.motion.xrel;
          mVirtualMouseY += evt.motion.yrel;
          warpCapturedMouseToCenter();
        } else {
          mVirtualMouseX = evt.motion.x;
          mVirtualMouseY = evt.motion.y;
        }

        stateMgr->injectMouseMotionInput(mVirtualMouseX, mVirtualMouseY);
        break;

      case SDL_EVENT_WINDOW_FOCUS_GAINED:
        if (!mCursorShown) {
          if (mWarpMouseCapture) {
            if (!SDL_SetWindowMouseGrab(mWindow, true)) {
              gLogger->warn(
                  format("Could not restore mouse grab: {}", SDL_GetError()));
            } else {
              warpCapturedMouseToCenter();
            }
          } else {
            // The initial request can precede compositor focus. Toggle it now
            // so backend rejection is observable and can select the fallback.
            SDL_SetWindowRelativeMouseMode(mWindow, false);
            captureMouse();
          }
        }
        break;

      case SDL_EVENT_WINDOW_RESIZED:
        mWidth = evt.window.data1;
        mHeight = evt.window.data2;
        break;

      case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      case SDL_EVENT_QUIT:
        mActive = false;
        break;
    }
  }
}
