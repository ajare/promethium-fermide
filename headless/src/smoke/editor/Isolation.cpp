#include "Checks.h"
#include "State.h"

#include "DocumentEdit.h"
#include "UISettings.h"
#include "imgui/imgui.h"

extern UISettings gUISettings;

namespace
{
	void normalAndExceptionalExit(smoke::Context const&)
	{
		editor_smoke::State outer;
		auto* caller = ImGui::GetCurrentContext();
		gUISettings.highlightNearestVertex = true;
		for (bool fail : { false, true })
		{
			try
			{
				editor_smoke::State inner;
				smoke::require(ImGui::GetCurrentContext() != caller,
					"Editor check reused the caller ImGui context");
				smoke::require(!ImGui::GetIO().IniFilename && !ImGui::GetIO().LogFilename,
					"Editor context permits persistent UI output");
				smoke::require(!gUISettings.highlightNearestVertex && !gWorldDocumentHistory.canUndo(),
					"Editor check inherited settings or history");
				gWorldDocumentHistory.commit(gWorldDocumentHistory.capture("temporary"));
				if (fail) throw smoke::Failure("intentional unwind");
			}
			catch (smoke::Failure const& error)
			{
				smoke::require(std::string_view(error.what()) == "intentional unwind",
					error.what());
			}
			smoke::require(ImGui::GetCurrentContext() == caller,
				"Editor cleanup did not restore the caller context");
			smoke::require(gUISettings.highlightNearestVertex && !gWorldDocumentHistory.canUndo(),
				"Editor cleanup leaked settings or history");
		}
	}
}

void editor_smoke::registerIsolation(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "isolation/normalAndExceptionalExit", normalAndExceptionalExit });
}
