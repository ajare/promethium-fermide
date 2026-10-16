#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include <willpower/application/State.h>
#include <willpower/application/StateFactory.h>
#include <willpower/common/Logger.h>

#include "StateManager.h"
#include "sdl/ImGuiSDL.h"
#include "sdl/WindowSDL.h"

wp::Logger* gLogger = nullptr;
bool gDisplayDebugEnabled = false;

namespace {
using namespace wp::application;

struct InputRecorder {
  std::vector<std::pair<KeyEvent, Key>> keyEvents;
  std::vector<KeyModifiers> keyModifiers;
  std::vector<std::pair<MouseButtonEvent, MouseButton>> mouseButtonEvents;
};

class RecordingState final : public State {
  InputRecorder& mRecorder;

  void injectKeyInputImpl(KeyEvent event, Key key, KeyModifiers modifiers) override {
    mRecorder.keyEvents.emplace_back(event, key);
    mRecorder.keyModifiers.push_back(modifiers);
  }

  void injectMouseButtonInputImpl(MouseButtonEvent event, MouseButton button, KeyModifiers) override {
    mRecorder.mouseButtonEvents.emplace_back(event, button);
  }

public:
  explicit RecordingState(InputRecorder& recorder) : State("RecordingState"), mRecorder(recorder) {}

  bool _imGuiActive() const override {
    return true;
  }

  bool _imGuiCapturesInput() const override {
    return false;
  }
};

class RecordingStateFactory final : public StateFactory {
  InputRecorder& mRecorder;

public:
  explicit RecordingStateFactory(InputRecorder& recorder) : StateFactory("RecordingState"), mRecorder(recorder) {}

  State* createState() override {
    return new RecordingState(mRecorder);
  }
};

void require(bool condition, std::string const& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void processEvent(WindowSDL& window, StateManager& stateManager, SDL_Event event) {
  require(SDL_PushEvent(&event), "Could not enqueue SDL test event.");
  window.processEvents(&stateManager);
}

SDL_Event keyEvent(Uint32 type, SDL_Keycode key, SDL_Keymod modifiers = SDL_KMOD_NONE) {
  SDL_Event event{};
  event.type = type;
  event.key.key = key;
  event.key.mod = modifiers;
  return event;
}

SDL_Event mouseButtonEvent(Uint32 type, uint8_t button) {
  SDL_Event event{};
  event.type = type;
  event.button.button = button;
  return event;
}

void finishImGuiFrame() {
  ImGui::EndFrame();
}

void requireKeyModifiers(WindowSDL& window, StateManager& stateManager, InputRecorder& recorder,
                         SDL_Keymod sdlModifiers, KeyModifiers expectedModifiers) {
  processEvent(window, stateManager, keyEvent(SDL_EVENT_KEY_DOWN, SDLK_A, sdlModifiers));
  require(recorder.keyModifiers.back() == expectedModifiers, "SDL modifier translation changed.");
  processEvent(window, stateManager, keyEvent(SDL_EVENT_KEY_UP, SDLK_A, sdlModifiers));
}
}  // namespace

int main() {
  try {
    require(SDL_Init(SDL_INIT_EVENTS), "Could not initialise SDL events.");

    ImGuiContext* context = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    ImGuiSdlData backendData;
    io.BackendPlatformUserData = &backendData;
    io.DisplaySize = ImVec2(100.0f, 100.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* fontPixels = nullptr;
    int fontWidth = 0, fontHeight = 0;
    io.Fonts->GetTexDataAsAlpha8(&fontPixels, &fontWidth, &fontHeight);

    InputRecorder recorder;
    RecordingStateFactory stateFactory(recorder);
    StateManager stateManager(nullptr, nullptr, nullptr, nullptr);
    stateManager.registerStateFactory(&stateFactory);
    stateManager.enterInitialState();

    ProgramOptions options{};
    WindowSDL window("input test", options);

    // Side buttons are not game controls, but their pressed and released
    // events still reach ImGui for back/forward navigation.
    processEvent(window, stateManager, mouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_X1));
    ImGui::NewFrame();
    require(ImGui::IsMouseDown(3), "ImGui did not receive side-button press.");
    finishImGuiFrame();

    processEvent(window, stateManager, mouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_X1));
    ImGui::NewFrame();
    require(!ImGui::IsMouseDown(3), "ImGui did not receive side-button release.");
    finishImGuiFrame();

    processEvent(window, stateManager, mouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_X2));
    ImGui::NewFrame();
    require(ImGui::IsMouseDown(4), "ImGui did not receive second side-button press.");
    finishImGuiFrame();

    processEvent(window, stateManager, mouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_X2));
    ImGui::NewFrame();
    require(!ImGui::IsMouseDown(4), "ImGui did not receive second side-button release.");
    finishImGuiFrame();

    processEvent(window, stateManager, mouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_X2 + 1));
    processEvent(window, stateManager, mouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_X2 + 1));
    require(recorder.mouseButtonEvents.empty(), "Unsupported mouse button reached the game input callback.");

    // An unknown key must not be default-inserted as Escape on either edge.
    processEvent(window, stateManager, keyEvent(SDL_EVENT_KEY_DOWN, SDLK_UNKNOWN));
    processEvent(window, stateManager, keyEvent(SDL_EVENT_KEY_UP, SDLK_UNKNOWN));
    require(recorder.keyEvents.empty(), "Unknown key reached the game input callback.");

    processEvent(window, stateManager, keyEvent(SDL_EVENT_KEY_DOWN, SDLK_A));
    processEvent(window, stateManager, keyEvent(SDL_EVENT_KEY_UP, SDLK_A));
    require(recorder.keyEvents == std::vector<std::pair<KeyEvent, Key>>{
                                      {KeyEvent::Pressed, Key::A},
                                      {KeyEvent::Released, Key::A}},
            "Supported key callbacks changed.");

    // The aggregate SDL masks overlap the left/right bits. Each side must map
    // only to its matching application bit rather than being added twice.
    requireKeyModifiers(window, stateManager, recorder, SDL_KMOD_LSHIFT, KeyModifiers::LeftShift);
    requireKeyModifiers(window, stateManager, recorder, SDL_KMOD_RSHIFT, KeyModifiers::RightShift);
    requireKeyModifiers(window, stateManager, recorder, SDL_KMOD_LCTRL, KeyModifiers::LeftCtrl);
    requireKeyModifiers(window, stateManager, recorder, SDL_KMOD_RCTRL, KeyModifiers::RightCtrl);
    requireKeyModifiers(window, stateManager, recorder, SDL_KMOD_LALT, KeyModifiers::LeftAlt);
    requireKeyModifiers(window, stateManager, recorder, SDL_KMOD_RALT, KeyModifiers::RightAlt);

    processEvent(window, stateManager, mouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT));
    processEvent(window, stateManager, mouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT));
    require(recorder.mouseButtonEvents == std::vector<std::pair<MouseButtonEvent, MouseButton>>{
                                              {MouseButtonEvent::Pressed, MouseButton::Left},
                                              {MouseButtonEvent::Released, MouseButton::Left}},
            "Supported mouse-button callbacks changed.");

    io.BackendPlatformUserData = nullptr;
    ImGui::DestroyContext(context);
    SDL_Quit();

    std::cout << "Launcher input callback tests passed\n";
    return 0;
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    SDL_Quit();
    return 1;
  }
}
