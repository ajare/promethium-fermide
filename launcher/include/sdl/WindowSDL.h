#pragma once

#include <map>

#include <SDL3/SDL.h>

#include <willpower/application/MouseButton.h>

#include "Window.h"
#include "StateManager.h"

class WindowSDL : public Window {
  SDL_Window* mWindow;

  SDL_GLContext mContextGL;

  float mContentScale;

  bool mActive;

  StateManager* mStateMgr;

  // While the cursor is captured SDL only reports relative motion, so the
  // absolute position the game consumes is accumulated here. GLFW's disabled
  // cursor did this for us.
  float mVirtualMouseX, mVirtualMouseY;

  // Native relative mode is the preferred unbounded FPS input. X11 without
  // XInput2 can reject it, so retain a grabbed, centre-warped fallback rather
  // than silently reverting to an edge-clamped absolute cursor.
  bool mCursorShown;
  bool mWarpMouseCapture;
  bool mIgnoreWarpMotion;

  // Input translation
  std::map<int, wp::application::Key> mKeyTranslator;

private:
  wp::application::KeyModifiers getKeyModifiers(uint16_t mod);

  bool translateKey(SDL_Keycode keycode, wp::application::Key& key) const;

  void warpCapturedMouseToCenter();

  void captureMouse();

public:
  WindowSDL(std::string const& title, ProgramOptions const& options);

  ~WindowSDL();

  SDL_Window* getWindow();

  float getContentScale() const;

  bool isActive() const;

  void create();

  void destroy();

  void setFullscreen(bool fullscreen);

  void setSize(int width, int height);

  void show();

  void showCursor(bool show);

  void setStateManager(StateManager* mgr);

  void processEvents(StateManager* stateMgr);
};
