#pragma once

#include <string>

#define APP_WINDOW_WIDTH 1440
#define APP_WINDOW_HEIGHT 810

void setWindowTitle(std::string const& title);

struct SDL_Cursor;

// Native feedback is visible even while a synchronous document operation
// prevents ImGui from presenting another frame. Restored on failure too.
class ApplicationBusyScope
{
	std::string mPreviousTitle;
	SDL_Cursor* mPreviousCursor{ nullptr };
	SDL_Cursor* mBusyCursor{ nullptr };
public:
	explicit ApplicationBusyScope(char const* operation);
	~ApplicationBusyScope();
	ApplicationBusyScope(ApplicationBusyScope const&) = delete;
	ApplicationBusyScope& operator=(ApplicationBusyScope const&) = delete;
};
