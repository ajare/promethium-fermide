#pragma once
#include "StationSystemsWindow.h"
#include "imgui/imgui.h"

// Session-only view selection. Neither visibility nor selection changes World state.
class ViewWindow
{
public:
	enum class Tab { World, StationSystems, StationFlow };
	Tab selectedTab() const { return mSelected; }
	bool flowModified() const { return mStation.flowModified(); }
	void initializeRecentStationFiles(std::filesystem::path path) { mStation.initializeRecentFiles(std::move(path)); }
	void setStationFilePicker(std::function<std::optional<std::string>(bool)> picker)
	{ mStation.setFilePicker(std::move(picker)); }
	void select(Tab tab)
	{
		if (tab != Tab::World && !mStationVisible) return;
		mRequested = tab; mSelectRequested = true;
	}
	bool stationSystemsVisible() const { return mStationVisible; }
	void setStationSystemsVisible(bool visible)
	{
		if (mStationVisible == visible) return;
		mStationVisible = visible;
		select(visible ? Tab::StationSystems : Tab::World);
	}

	template<class RenderWorld>
	void render(std::shared_ptr<core::World> const& world, DocumentHistory& history,
		RenderWorld renderWorld)
	{
		if (ImGui::Begin("View", nullptr,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
		{
			if (ImGui::BeginTabBar("Views"))
			{
				auto flags = [&](Tab tab) -> ImGuiTabItemFlags
				{ return mSelectRequested && mRequested == tab ? ImGuiTabItemFlags_SetSelected : 0; };
				if (ImGui::BeginTabItem("World", nullptr, flags(Tab::World)))
				{
					mSelected = Tab::World;
					ImGui::BeginChild("WorldCanvas", {0, 0}, false,
						ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
					if (world) renderWorld();
					else
					{
						ImGui::TextDisabled("No World is open.");
						ImGui::TextUnformatted("Choose File > New or File > Open to begin.");
					}
					ImGui::EndChild();
					ImGui::EndTabItem();
				}
				if (mStationVisible && ImGui::BeginTabItem("Station Systems", nullptr, flags(Tab::StationSystems)))
				{
					mSelected = Tab::StationSystems;
					ImGui::BeginChild("StationSystems", {0, 0});
					mStation.renderContents(world, history);
					ImGui::EndChild();
					ImGui::EndTabItem();
				}
				if (mStationVisible && ImGui::BeginTabItem("Station Flow", nullptr, flags(Tab::StationFlow)
					| (mStation.flowModified() ? ImGuiTabItemFlags_UnsavedDocument : 0)))
				{
					mSelected = Tab::StationFlow;
					mStation.renderFlowContents(world);
					ImGui::EndTabItem();
				}
				mSelectRequested = false;
				ImGui::EndTabBar();
			}
		}
		ImGui::End();
	}
private:
	Tab mSelected{Tab::World};
	Tab mRequested{Tab::World};
	bool mSelectRequested{false};
	bool mStationVisible{false};
	StationSystemsWindow mStation;
};
