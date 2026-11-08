#define NOMINMAX

#include "core/AgentType.h"

#if defined(_WIN32)
#include <Windows.h>
#include <GL/glew.h>
#include <nfd.h>
#elif defined(__linux__)
#include <GL/glew.h>
#include <nfd.h>
#include <unistd.h>
#include <array>
#include <chrono>
#include <stdexcept>
#else
#error "Unsupported platform"
#endif

#include <filesystem>
#include <fstream>
#include "Helpers.h"
#include "NonInteractiveProcess.h"
#include "WorldRenderSystem.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4307)
#endif
#include <spdlog/spdlog.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/callback_sink.h>
#include <spdlog/details/log_msg_buffer.h>

#if defined(_WIN32)
#include <SDL2/SDL.h>
#if defined(IMGUI_IMPL_OPENGL_ES2)
#include <SDL2/SDL_opengles2.h>
#else
#include <SDL2/SDL_opengl.h>
#endif
#elif defined(__linux__)
#include <SDL.h>
#if defined(IMGUI_IMPL_OPENGL_ES2)
#include <SDL_opengles2.h>
#else
#include <SDL_opengl.h>
#endif
#else
#error "Unsupported platform"
#endif

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/imgui_impl_sdl2.h"
#include "imgui/imgui_impl_opengl3.h"
#include "imgui/imnodes.h"
#include "imgui/IconsFontAwesome5.h"

#include "core/World.h"
#include "core/Pathing.h"
#include "core/Exceptions.h"
#include "core/Log.h"

#include "EditorDocument.h"
#include "Main.h"
#include "Render.h"
#include "Helpers.h"
#include "NonInteractiveProcess.h"
#include "UI.h"
#include "UISettings.h"
#include "metrics/MetricsHttpServer.h"
#include "Exceptions.h"


spdlog::logger* gLogger{ nullptr };
SDL_Window* gWindow{ nullptr };
SDL_GLContext gContext{ nullptr };
UISettings gUISettings;
bool gMetricsDetail{ false };

ImFont* gAgentIconFont{ nullptr };
std::filesystem::path gResourceDirectory;

namespace
{
	// Tracks which subsystems actually finished initialising so shutdown can
	// release only what exists after a partial-startup failure (#59).
	bool gSdlInitialised{ false };
	bool gImGuiContextCreated{ false };
	bool gImGuiSdlBackendInitialised{ false };
	bool gImGuiOpenGLBackendInitialised{ false };
	bool gNfdInitialised{ false };
}

using namespace std;

namespace
{
	string trim(string value)
	{
		auto const first = value.find_first_not_of(" \t\r\n");
		if (first == string::npos) return {};
		auto const last = value.find_last_not_of(" \t\r\n");
		return value.substr(first, last - first + 1);
	}

	filesystem::path executableDirectory()
	{
#if defined(_WIN32)
		wstring executablePath(MAX_PATH, L'\0');
		auto const length = GetModuleFileNameW(nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
		if (length == 0 || length == executablePath.size())
		{
			throw ExitApplicationException(1, "Could not determine the executable directory.");
		}
		executablePath.resize(length);
		return filesystem::path(executablePath).parent_path();
#elif defined(__linux__)
		array<char, 4096> executablePath{};
		auto const length = readlink("/proc/self/exe", executablePath.data(), executablePath.size() - 1);
		if (length <= 0 || static_cast<size_t>(length) >= executablePath.size() - 1)
		{
			throw ExitApplicationException(1, "Could not determine the executable directory.");
		}
		return filesystem::path(string(executablePath.data(), static_cast<size_t>(length))).parent_path();
#else
#error "Unsupported platform"
#endif
	}

	filesystem::path loadResourceDirectory()
	{
		auto const configurationPath = executableDirectory() / "promethium-fermide.ini";
		ifstream configuration(configurationPath);
		if (!configuration)
		{
			throw ExitApplicationException(1, "Could not open ImGui configuration: " + configurationPath.string());
		}

		string section;
		string resourceDirectory;
		for (string line; getline(configuration, line);)
		{
			line = trim(line);
			if (line.empty() || line.starts_with(';') || line.starts_with('#')) continue;
			if (line.front() == '[' && line.back() == ']')
			{
				section = trim(line.substr(1, line.size() - 2));
				continue;
			}

			auto const separator = line.find('=');
			if (section == "Config" && separator != string::npos
				&& trim(line.substr(0, separator)) == "ResourceDir")
			{
				resourceDirectory = trim(line.substr(separator + 1));
			}
		}

		if (resourceDirectory.empty())
		{
			throw ExitApplicationException(1, "ImGui configuration does not define Config.ResourceDir.");
		}

		filesystem::path resourcePath(resourceDirectory);
		if (resourcePath.is_relative()) resourcePath = executableDirectory() / resourcePath;
		resourcePath = resourcePath.lexically_normal();
		if (!filesystem::is_directory(resourcePath))
		{
			throw ExitApplicationException(1, "Configured resource directory does not exist: " + resourcePath.string());
		}
		return resourcePath;
	}
}

void setWindowTitle(string const& title)
{
	if (gWindow) SDL_SetWindowTitle(gWindow, title.c_str());
}

ApplicationBusyScope::ApplicationBusyScope(char const* operation)
{
	if (!gWindow) return;
	mPreviousTitle = SDL_GetWindowTitle(gWindow);
	mPreviousCursor = SDL_GetCursor();
	mBusyCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_WAIT);
	if (mBusyCursor) SDL_SetCursor(mBusyCursor);
	SDL_SetWindowTitle(gWindow, (mPreviousTitle + " - " + operation).c_str());
}

ApplicationBusyScope::~ApplicationBusyScope()
{
	if (gWindow) SDL_SetWindowTitle(gWindow, mPreviousTitle.c_str());
	if (mBusyCursor)
	{
		SDL_SetCursor(mPreviousCursor);
		SDL_FreeCursor(mBusyCursor);
	}
}

SDL_Window* createWindow()
{
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0)
	{
		throw ExitApplicationException(1, "SDL initialisation failed: " + string(SDL_GetError()));
	}
	gSdlInitialised = true;

	// GL 3.0 + GLSL 130
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

	// From 2.0.18: Enable native IME.
#ifdef SDL_HINT_IME_SHOW_UI
	SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
#endif

	// Create window with graphics context
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
	SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
	SDL_Window* window = SDL_CreateWindow("ImGui PF Engine", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, APP_WINDOW_WIDTH, APP_WINDOW_HEIGHT, window_flags);
	if (window == nullptr)
	{
		throw ExitApplicationException(1, "SDL window creation failed: " + string(SDL_GetError()));
	}

	gLogger->info("Window created");

	return window;
}

SDL_GLContext createContext(SDL_Window* window)
{
	SDL_GLContext gl_context = SDL_GL_CreateContext(window);
	if (gl_context == nullptr)
	{
		throw ExitApplicationException(1, "OpenGL context creation failed: " + string(SDL_GetError()));
	}

	if (SDL_GL_MakeCurrent(window, gl_context) != 0)
	{
		SDL_GL_DeleteContext(gl_context);
		throw ExitApplicationException(1, "Could not make the OpenGL context current: " + string(SDL_GetError()));
	}

	if (SDL_GL_SetSwapInterval(1) != 0) // Enable vsync
	{
		gLogger->warn("Could not enable vsync: " + string(SDL_GetError()));
	}

	gLogger->info("OpenGL context created");

	return gl_context;
}

void setupImGui(SDL_Window* window, SDL_GLContext context)
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiSettingsHandler metricsSettings;
	metricsSettings.TypeName = "Metrics";
	metricsSettings.TypeHash = ImHashStr("Metrics");
	metricsSettings.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler*, char const*) -> void* { return &gUISettings; };
	metricsSettings.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*, void*, char const* line) {
		int value;
		if (sscanf(line, "Enabled=%d", &value) == 1) gUISettings.metricsEnabled = value != 0;
		if (sscanf(line, "Port=%d", &value) == 1 && value > 0 && value <= 65535) gUISettings.metricsPort = value;
	};
	metricsSettings.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler*, ImGuiTextBuffer* buffer) {
		buffer->appendf("[Metrics][Settings]\nEnabled=%d\nPort=%d\n\n", gUISettings.metricsEnabled ? 1 : 0, gUISettings.metricsPort);
	};
	ImGui::AddSettingsHandler(&metricsSettings);
	gImGuiContextCreated = true;
	ImNodes::CreateContext();
	ImNodes::PushAttributeFlag(ImNodesAttributeFlags_EnableLinkDetachWithDragClick);

	// Setup Dear ImGui style
	switch (gUISettings.style)
	{
	case UISettings::Light:
		ImGui::StyleColorsLight();
		break;

	case UISettings::Dark:
		ImGui::StyleColorsDark();
		break;

	case UISettings::Classic:
		ImGui::StyleColorsClassic();
		break;
	}

	// Setup Platform/Renderer backends
	ImGui_ImplSDL2_InitForOpenGL(window, context);
	gImGuiSdlBackendInitialised = true;

	const char* glsl_version = "#version 130";
	if (!ImGui_ImplOpenGL3_Init(glsl_version))
	{
		throw ExitApplicationException(1, "ImGui OpenGL3 backend initialisation failed; the GLSL version is unsupported.");
	}
	gImGuiOpenGLBackendInitialised = true;

	// Load Fonts
	// - If no fonts are loaded, dear imgui will use the default font. You can also load multiple fonts and use ImGui::PushFont()/PopFont() to select them.
	// - AddFontFromFileTTF() will return the ImFont* so you can store it if you need to select the font among multiple.
	// - If the file cannot be loaded, the function will return NULL. Please handle those errors in your application (e.g. use an assertion, or display an error and quit).
	// - The fonts will be rasterized at a given size (w/ oversampling) and stored into a texture when calling ImFontAtlas::Build()/GetTexDataAsXXXX(), which ImGui_ImplXXXX_NewFrame below will call.
	// - Use '#define IMGUI_ENABLE_FREETYPE' in your imconfig file to use Freetype for higher quality font rendering.
	// - Read 'docs/FONTS.md' for more instructions and details.
	// - Remember that in C/C++ if you want to include a backslash \ in a string literal you need to write a double backslash \\ !
	// - Our Emscripten build process allows embedding fonts to be accessible at runtime from the "fonts/" folder. See Makefile.emscripten for details.
	//io.Fonts->AddFontDefault();
	//io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\segoeui.ttf", 18.0f);
	//io.Fonts->AddFontFromFileTTF("../../misc/fonts/DroidSans.ttf", 16.0f);
	//io.Fonts->AddFontFromFileTTF("../../misc/fonts/Roboto-Medium.ttf", 16.0f);
	//io.Fonts->AddFontFromFileTTF("../../misc/fonts/Cousine-Regular.ttf", 15.0f);
	//ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\ArialUni.ttf", 18.0f, NULL, io.Fonts->GetGlyphRangesJapanese());
	//IM_ASSERT(font != NULL);

	gLogger->info("ImGui set up");
}

auto callbackSink = std::make_shared<spdlog::sinks::callback_sink_mt>([](const spdlog::details::log_msg& msg)
{
	core::LogLevel level;
	switch (msg.level)
	{
	case spdlog::level::trace:
	case spdlog::level::debug:
		level = core::LogLevel::Debug;
		break;

	case spdlog::level::info:
		level = core::LogLevel::Info;
		break;

	case spdlog::level::warn:
		level = core::LogLevel::Warning;
		break;

	case spdlog::level::err:
	case spdlog::level::critical:
	default:
		level = core::LogLevel::Error;
		break;
	}

	core::addLogMessage("Application", ~0u, level,
		string(msg.payload.data(), msg.payload.size()));
});

void setupLogging()
{
#ifdef _DEBUG
	filesystem::path const logPath("../../../logs/pf-debug.log");
#else
	filesystem::path const logPath("../../../logs/pf-release.log");
#endif

	// A missing log directory must not take the whole application down before
	// the startup handler is in place; create it if possible and let spdlog
	// report a genuinely unwritable destination as a controlled error.
	std::error_code ec;
	filesystem::create_directories(logPath.parent_path(), ec);

	auto fileSink = make_shared<spdlog::sinks::basic_file_sink_mt>(logPath.string(), true);

#ifdef _DEBUG
	auto consoleSink = make_shared<spdlog::sinks::stdout_color_sink_mt>();
	consoleSink->set_level(spdlog::level::debug);

	fileSink->set_level(spdlog::level::debug);

	gLogger = new spdlog::logger("pf", { consoleSink, fileSink, callbackSink });
#else
	fileSink->set_level(spdlog::level::info);

	gLogger = new spdlog::logger("editor", { fileSink, callbackSink });
#endif

	gLogger->set_level(spdlog::level::debug);
}

void initialise()
{
	setupLogging();

	//
	// Set up SDL
	//
	gWindow = createWindow();
	gContext = createContext(gWindow);

	glewExperimental = GL_TRUE;
	if (glewInit() != GLEW_OK)
	{
#if defined(_WIN32)
		throw exception("GLEW initialisation failed");
#elif defined(__linux__)
		throw runtime_error("GLEW initialisation failed");
#else
#error "Unsupported platform"
#endif
	}

	//
	// Set up ImGui
	//
	setupImGui(gWindow, gContext);
}

void setup()
{
	gResourceDirectory = loadResourceDirectory();
	int drawableWidth{}, drawableHeight{};
	SDL_GL_GetDrawableSize(gWindow, &drawableWidth, &drawableHeight);
	try
	{
		createWorldRenderSystem(gResourceDirectory,
			static_cast<std::size_t>(drawableWidth),
			static_cast<std::size_t>(drawableHeight));
	}
	catch (std::exception const& error)
	{
		throw ExitApplicationException(1,
			"Could not initialise Willpower resources and MPP rendering: "
			+ std::string(error.what()));
	}
	initializeRecentFiles(executableDirectory() / "recent-files.txt");

	// Set up NFD (file dialogs)
	if (NFD_Init() != NFD_OKAY)
	{
		throw ExitApplicationException(1, "Native file dialog initialisation failed: " + string(NFD_GetError()));
	}
	gNfdInitialised = true;

	// ImGui extra twiddling
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;         // Enable the docking branch UI

	io.Fonts->AddFontDefault();

	float baseFontSize = 13.0f; // 13.0f is the size of the default font. Change to the font size you use.
	float iconFontSize = baseFontSize * 2.0f / 3.0f; // FontAwesome fonts need to have their sizes reduced by 2.0f/3.0f in order to align correctly

	static const ImWchar icons_ranges[] = { ICON_MIN_FA, ICON_MAX_16_FA, 0 };
	ImFontConfig icons_config;
	icons_config.MergeMode = true;
	icons_config.PixelSnapH = true;
	icons_config.GlyphMinAdvanceX = iconFontSize;
	auto const iconFontPath = (gResourceDirectory / FONT_ICON_FILE_NAME_FAS).string();
	io.Fonts->AddFontFromFileTTF(iconFontPath.c_str(), iconFontSize, &icons_config, icons_ranges);

	// Palette items and world entities are rendered much larger than toolbar icons.
	// Rasterize their glyphs at Agent height instead of enlarging the merged bitmap.
	static const ImWchar agentIconRanges[] = {
		0xf183, 0xf183, // Male
		0xf21d, 0xf21d, // Street View
		0xf2d0, 0xf2d0, // Window Maximize
		0xf3c5, 0xf3c5, // Map Marker Alt
		0xf52a, 0xf52a, // Door Closed
		0xf7a4, 0xf7a4, // Grip Lines (Walkway)
		0
	};
	gAgentIconFont = io.Fonts->AddFontFromFileTTF(iconFontPath.c_str(),
		core::Agent::placementDimensions(core::bundledHumanBaseline()).y * CORE_LEVEL_HEIGHT_PIXELS, nullptr, agentIconRanges);
	if (!gAgentIconFont)
	{
		throw ExitApplicationException(1, "Could not load the Agent icon font.");
	}

	io.Fonts->Build();
	if (!gAgentIconFont->FindGlyphNoFallback(0xf7a4))
	{
		throw ExitApplicationException(1, "Could not load the Walkway palette icon.");
	}
}

void shutdown()
{
	if (gLogger)
	{
		gLogger->info("Shutting down");
	}

	// MPP and Willpower own GPU resources and must be destroyed while the GL
	// context which created them is still current.
	destroyWorldRenderSystem();

	// ImGui backends, newest initialised first; skip anything that never came up.
	if (gImGuiOpenGLBackendInitialised)
	{
		ImGui_ImplOpenGL3_Shutdown();
		gImGuiOpenGLBackendInitialised = false;
	}

	if (gImGuiSdlBackendInitialised)
	{
		ImGui_ImplSDL2_Shutdown();
		gImGuiSdlBackendInitialised = false;
	}

	if (gImGuiContextCreated)
	{
		ImGui::DestroyContext();
		gImGuiContextCreated = false;
	}

	// Platform
	if (gContext != nullptr)
	{
		SDL_GL_DeleteContext(gContext);
		gContext = nullptr;
	}

	if (gWindow != nullptr)
	{
		SDL_DestroyWindow(gWindow);
		gWindow = nullptr;
	}

	if (gSdlInitialised)
	{
		SDL_Quit();
		gSdlInitialised = false;
	}

	if (gNfdInitialised)
	{
		NFD_Quit();
		gNfdInitialised = false;
	}

	delete gLogger;
	gLogger = nullptr;
}

bool processEvents(SDL_Window* window)
{
	// Poll and handle events (inputs, window resize, etc.)
	// You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
	// - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or clear/overwrite your copy of the mouse data.
	// - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or clear/overwrite your copy of the keyboard data.
	// Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
	bool done = false;
	SDL_Event event;
	while (SDL_PollEvent(&event))
	{
		ImGui_ImplSDL2_ProcessEvent(&event);
		if (event.type == SDL_QUIT)
		{
			done = true;
		}
		if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window))
		{
			done = true;
		}
	}

	return done;
}


void run()
{
	// The editor holds its authoring handle, not a World. What it holds is the
	// open document, which owns the runtime that drives the preview and the
	// `WorldAuthoring` that changes the document; the World only ever reaches
	// the UI as the aliasing view that handle grants (#972).
	editor::DocumentPtr document;// opened by File > New or File > Open below
	// The metrics collector follows the open document rather than the World, so
	// its observer is registered on the runtime that actually drives the
	// preview (#971).
	std::shared_ptr<editor::EditorDocument> metricsDocument;
	std::unique_ptr<core::SimulationMetricsCollector> metricsCollector;
	std::unique_ptr<metrics::MetricsHttpServer> metricsServer;
	int metricsPort = -1;
	struct ObserverCleanup
	{
		std::shared_ptr<editor::EditorDocument>& observedDocument;
		~ObserverCleanup()
		{
			if (observedDocument)
				observedDocument->runtime().setSimulationObserver(nullptr);
		}
	} observerCleanup{metricsDocument};
	// The document is closed when the frame loop ends rather than being left to
	// static destruction: the World and the runtime that owns it should be torn
	// down while the process is still in a state to tear them down (#971).
	struct DocumentCleanup
	{
		~DocumentCleanup() { editor::closeDocument(); }
	} documentCleanup;
	std::shared_ptr<core::Agent> pathingAgent = core::Agent::create(core::bundledHumanAgentType(), "Pather");

	// Render settings
	ImVec4 clearColour = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

	//
	// Main loop
	//
#if defined(_WIN32)
	LARGE_INTEGER startingTime, endingTime, elapsedMicroseconds;
	LARGE_INTEGER frequency;
	QueryPerformanceFrequency(&frequency);
	QueryPerformanceCounter(&startingTime);
#elif defined(__linux__)
	auto previousTime = chrono::steady_clock::now();
#else
#error "Unsupported platform"
#endif

	bool done = false, showDemoWindow = false;
	while (!done)
	{
		// Get elapsed time
#if defined(_WIN32)
		QueryPerformanceCounter(&endingTime);
		elapsedMicroseconds.QuadPart = endingTime.QuadPart - startingTime.QuadPart;
		elapsedMicroseconds.QuadPart *= 1000000;
		elapsedMicroseconds.QuadPart /= frequency.QuadPart;
		startingTime = endingTime;
		auto const updateTimeMicros = elapsedMicroseconds.QuadPart;
#elif defined(__linux__)
		auto const currentTime = chrono::steady_clock::now();
		auto const updateTimeMicros = chrono::duration_cast<chrono::microseconds>(
			currentTime - previousTime).count();
		previousTime = currentTime;
#else
#error "Unsupported platform"
#endif

		// Events. Window-manager close requests are resolved after NewFrame so the
		// UI can open the same unsaved-changes confirmation used by File actions.
		bool const closeRequested = processEvents(gWindow);

		// Logic
		float updateTimeSecs = updateTimeMicros / 1'000'000.0f;

		auto const open = editor::currentDocument();
		if (metricsDocument != open || metricsPort != gUISettings.metricsPort || (!gUISettings.metricsEnabled && metricsCollector))
		{
			if (metricsDocument) metricsDocument->runtime().setSimulationObserver(nullptr);
			metricsServer.reset(); metricsCollector.reset(); metricsDocument.reset();
			metricsPort = gUISettings.metricsPort;
		}
		if (gUISettings.metricsEnabled && open && !metricsCollector)
		{
			metricsDocument = open;
			metricsCollector = std::make_unique<core::SimulationMetricsCollector>(
				metricsDocument->runtime().world(), gMetricsDetail);
			metricsServer = std::make_unique<metrics::MetricsHttpServer>(*metricsCollector);
			if (!metricsServer->start(metricsPort)) fprintf(stderr, "%s\n", metricsServer->diagnostic().c_str());
			metricsDocument->runtime().setSimulationObserver(metricsCollector.get());
		}
		if (metricsCollector && (gUISettings.worldPaused || metricsDocument->runtime().isSimulationPaused())) metricsCollector->refresh();
		if (document)
		{
			// The preview is ticked by the same WorldRuntime the shipped game
			// ticks, and its event queue is drained here exactly as the game's
			// would be (#971). The runtime is the document's own, taken through
			// the authoring handle the document was built with, so there is no
			// lookup that could pick up a stale one.
			auto& runtime = document->runtime();
			runtime.update(gUISettings.worldPaused ? 0.0f : updateTimeSecs);
			// The current UI observes entity state directly. Drain value events until
			// an event-driven UI consumer is introduced so the queue remains bounded.
			(void)runtime.consumeSimulationEvents();
		}

		// Set up rendering
		int drawableWidth, drawableHeight;
		SDL_GL_GetDrawableSize(gWindow, &drawableWidth, &drawableHeight);
		resizeWorldRenderSystem(static_cast<std::size_t>(drawableWidth),
			static_cast<std::size_t>(drawableHeight));
		glViewport(0, 0, drawableWidth, drawableHeight);
		glClearColor(clearColour.x * clearColour.w, clearColour.y * clearColour.w, clearColour.z * clearColour.w, clearColour.w);
		glClear(GL_COLOR_BUFFER_BIT);
		glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);

		// Start the Dear ImGui frame
		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplSDL2_NewFrame();

		ImGui::NewFrame();

		if (closeRequested) done = requestApplicationClose(document);
		handleShortcuts(document);
		if (document) handleContinuousKeyboardInput(document, updateTimeMicros);

		if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_F11)))
		{
			showDemoWindow = !showDemoWindow;
		}

		if (showDemoWindow)
		{
			ImGui::SetNextWindowFocus();
			ImGui::ShowDemoWindow();
		}

		renderUI(document, pathingAgent);

		if (document)
		{
			auto mouseButtonStatus = getMouseButtonStatus();
			handleWorldInteraction(document, document->world()->getGraph(), mouseButtonStatus);
		}

		// Rendering
		ImGui::Render();

		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
		SDL_GL_SwapWindow(gWindow);
	}
}


void outputToDebugger(std::string const& msg)
{
#if defined(_DEBUG) && defined(_WIN32)
	char const* msgc = msg.c_str();

	size_t reqLength = ::MultiByteToWideChar(CP_UTF8, 0, msgc, (int)strlen(msgc), 0, 0);
	std::wstring ret(reqLength, L'\0');

	::MultiByteToWideChar(CP_UTF8, 0, msgc, (int)strlen(msgc), &ret[0], (int)ret.length());
	OutputDebugString(ret.c_str());
#else
	(void)msg;
#endif
}


void outputException(std::string const& msg)
{
	if (gLogger)
	{
		gLogger->critical(msg);
	}
	else
	{
		// Startup can fail before or during logging setup, so keep a fallback
		// channel for the diagnostic.
		fprintf(stderr, "Critical: %s\n", msg.c_str());
	}

	outputToDebugger(msg);
}


//
// Entrypoint
//
int main(int argc, char** argv)
{
	pf::setupNonInteractiveProcess();
	int exitCode{ 0 };

	try
	{
		initialise();
		setup();
		ImGui::LoadIniSettingsFromDisk(ImGui::GetIO().IniFilename);
		for (int i = 1; i < argc; ++i)
		{
			if (std::string(argv[i]) == "--metrics-detail=sector,queue") gMetricsDetail = true;
			if (std::string(argv[i]) == "--metrics-port" && i + 1 < argc)
			{
				auto port = std::stoi(argv[++i]);
				if (port < 1 || port > 65535) throw std::invalid_argument("Invalid metrics port");
				gUISettings.metricsPort = port; gUISettings.metricsEnabled = true;
			}
		}
		run();
	}
	catch (ExitApplicationException& e)
	{
		exitCode = e.getExitCode();
		if (exitCode != 0)
		{
			outputException(e.what());
		}
	}
	catch (exception& e)
	{
		exitCode = 1;
		outputException(e.what());
	}

	shutdown();
	
	return exitCode;
}
