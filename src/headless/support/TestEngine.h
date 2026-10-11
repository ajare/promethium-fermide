#pragma once

#include <functional>
#include <string>

#include "imgui_test_engine/imgui_te_context.h"

namespace headless
{
	// Drives one Dear ImGui Test Engine test to completion against a
	// caller-supplied frame renderer. Owns the engine context, the captured
	// frame text and the assertion counters, so a migrated check reads much
	// like the original scan-based one.
	//
	// Two rules this encodes, both learned the hard way during the spike:
	//
	//   * Text is captured with ImGui::LogToBuffer(), not LogToClipboard().
	//     The engine replaces io.SetClipboardTextFn while a test runs, which
	//     silently empties clipboard-based capture.
	//
	//   * clickDisabled() performs a real mouse press at the resolved item's
	//     coordinates. ItemClick() silently skips disabled targets and still
	//     reports Success, which would make a "clicking while disabled must
	//     not mutate" assertion vacuous.
	class TestEngineDriver
	{
	public:
		using WidgetRender = std::function<void ()>;
		using TestBody = std::function<void (ImGuiTestContext*)>;

		explicit TestEngineDriver(WidgetRender renderWidgets);
		~TestEngineDriver();
		TestEngineDriver(TestEngineDriver const&) = delete;
		TestEngineDriver& operator=(TestEngineDriver const&) = delete;

		// Render exactly one frame, capturing widget text into text().
		void frame();
		// Run one test body to completion, counting frames rendered.
		void run(char const* category, char const* name, TestBody body);
		// Let the engine observe frames. Required before the first run(), or
		// QueueTest marks the test as errored against a stale frame count.
		void warmup(int frames = 4);

		// Project-style assertion: records the failure and returns false rather
		// than throwing, so the caller can bail out of the test body.
		bool require(bool condition, char const* message);
		bool contains(char const* needle) const;

		// Resolve an item, assert it is disabled, then really press it. Returns
		// false if the item did not resolve or was not disabled.
		bool clickDisabled(ImGuiTestContext* ctx, ImGuiTestRef ref, char const* what);

		// Canonical click. The engine's item hooks go idle between operations,
		// and the first resolve after that gap returns geometry that is one
		// frame stale. When a previous action changed the layout - adding or
		// removing a row shifts everything below it - clicking straight away
		// lands on the old position. Sleeping one frame first makes the
		// resolve see current geometry.
		void click(ImGuiTestContext* ctx, ImGuiTestRef ref);
		// Same settle before typing into a field.
		void typeValue(ImGuiTestContext* ctx, ImGuiTestRef ref, char const* value);
		// Resolve an item and return its centre, for raw-position operations.
		bool itemCenter(ImGuiTestContext* ctx, ImGuiTestRef ref, float* x, float* y);

		std::string const& text() const { return text_; }
		long long frames() const { return frames_; }
		int assertions() const { return assertions_; }
		int failures() const { return failures_; }
		bool passed() const { return failures_ == 0 && completed_; }

	private:
		struct Impl;
		Impl* impl_;
		WidgetRender renderWidgets_;
		std::string text_;
		long long frames_{ 0 };
		int assertions_{ 0 };
		int failures_{ 0 };
		bool completed_{ false };
	};
}
