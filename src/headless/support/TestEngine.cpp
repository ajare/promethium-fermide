#include "TestEngine.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui_test_engine/imgui_te_engine.h"
#include "imgui_test_engine/imgui_te_context.h"

#include <cstdio>

namespace headless
{
	namespace
	{
		// Guard against a wedged test body spinning the driver forever.
		constexpr long long kFrameCap = 40000;
	}

	struct TestEngineDriver::Impl
	{
		ImGuiTestEngine* engine{ nullptr };
		ImGuiTest* currentTest{ nullptr };
		bool stuck{ false };
	};

	TestEngineDriver::TestEngineDriver(WidgetRender renderWidgets)
		: impl_(new Impl()), renderWidgets_(std::move(renderWidgets))
	{
		impl_->engine = ImGuiTestEngine_CreateContext();
		ImGuiTestEngineIO& tio = ImGuiTestEngine_GetIO(impl_->engine);
		tio.ConfigVerboseLevel = ImGuiTestVerboseLevel_Warning;
		tio.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Debug;
		tio.ConfigRunSpeed = ImGuiTestRunSpeed_Fast;
		ImGuiTestEngine_Start(impl_->engine, ImGui::GetCurrentContext());
	}

	TestEngineDriver::~TestEngineDriver()
	{
		if (impl_ && impl_->engine)
		{
			ImGuiTestEngine_Stop(impl_->engine);
			ImGuiTestEngine_DestroyContext(impl_->engine);
			impl_->engine = nullptr;
		}
		delete impl_;
	}

	void TestEngineDriver::frame()
	{
		ImGui::NewFrame();
		// Capture through the clipboard hook rather than LogToBuffer(). ImGui
		// 1.89.5's LogToBuffer() leaves the buffer empty in this setup, while
		// the clipboard path is proven to fill. The engine replaces
		// io.SetClipboardTextFn while a test runs (imgui_te_engine.cpp:1405)
		// for its own "copy log to clipboard" feature, so ours is re-asserted
		// every frame; the engine's feature is unused here.
		ImGuiIO& io = ImGui::GetIO();
		io.SetClipboardTextFn = [](void* data, char const* value)
		{ *static_cast<std::string*>(data) = value; };
		io.ClipboardUserData = &text_;
		text_.clear();
		ImGui::LogToClipboard();
		if (renderWidgets_)
			renderWidgets_();
		ImGui::LogFinish();
		ImGui::Render();
		++frames_;
	}

	void TestEngineDriver::warmup(int frames)
	{
		for (int i = 0; i < frames; ++i)
			frame();
	}

	void TestEngineDriver::run(char const* category, char const* name, TestBody body)
	{
		ImGuiTest* test = IM_REGISTER_TEST(impl_->engine, category, name);
		test->TestFunc = [body](ImGuiTestContext* ctx) { body(ctx); };
		impl_->currentTest = test;
		ImGuiTestEngine_QueueTest(impl_->engine, test);
		long long const start = frames_;
		while (test->Status == ImGuiTestStatus_Queued
			|| test->Status == ImGuiTestStatus_Running)
		{
			frame();
			if (frames_ - start > kFrameCap)
			{
				impl_->stuck = true;
				++failures_;
				std::printf("    FAIL: test '%s' exceeded the frame cap\n", name);
				break;
			}
		}
		completed_ = !impl_->stuck && test->Status == ImGuiTestStatus_Success;
	}

	bool TestEngineDriver::require(bool condition, char const* message)
	{
		++assertions_;
		if (!condition)
		{
			++failures_;
			std::printf("    FAIL: %s\n", message ? message : "(no message)");
		}
		return condition;
	}

	bool TestEngineDriver::contains(char const* needle) const
	{
		return text_.find(needle) != std::string::npos;
	}

	bool TestEngineDriver::itemCenter(ImGuiTestContext* ctx, ImGuiTestRef ref,
		float* x, float* y)
	{
		ImGuiTestItemInfo* info = ctx->ItemInfo(ref);
		if (!require(!info->IsEmpty(), "item did not resolve"))
			return false;
		auto const center = info->RectFull.GetCenter();
		if (x) *x = center.x;
		if (y) *y = center.y;
		return true;
	}

	bool TestEngineDriver::clickDisabled(ImGuiTestContext* ctx, ImGuiTestRef ref,
		char const* what)
	{
		ImGuiTestItemInfo* info = ctx->ItemInfo(ref);
		if (!require(!info->IsEmpty(), "disabled target did not resolve"))
			return false;
		// Assert the control really is disabled. The original tests only
		// inferred this from "nothing happened"; asserting it directly makes
		// the check strictly stronger, and guards against the case where the
		// control silently became enabled and the test stopped proving anything.
		if (!require((info->InFlags & ImGuiItemFlags_Disabled) != 0,
			"control is not disabled where a disabled click was expected"))
			return false;
		// A real press at the resolved coordinates. ItemClick() would skip a
		// disabled target and still report Success.
		ctx->MouseTeleportToPos(info->RectFull.GetCenter());
		ctx->MouseClick();
		ctx->Sleep(2);
		(void)what;
		return true;
	}
}
