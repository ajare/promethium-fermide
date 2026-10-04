#include "Checks.h"
#include "State.h"
#include "BrokenExtensibleFixture.h"
#include "DoorPanel.h"
#include "DocumentEdit.h"
#include "core/YamlSerializer.h"
#include "core/Button.h"
#include <algorithm>
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

namespace
{
	void bridgeInsetHistory()
	{
		using smoke::require;
		auto world = std::make_shared<core::World>("Bridge document insets", 12, 3);
		auto room = world->addRoom("Room", 0, 0, 0, 12, 3);
		for (auto x : {0u, 1u, 4u, 5u, 8u, 9u, 10u, 11u}) world->addSectorWalkway(room, 1, x);
		world->finishBuild(); world->pauseSimulation();
		DocumentHistory history;
		auto edit = [&](auto action)
		{
			auto before = captureDocumentSnapshot(world, history); action(); world->finishBuild(); world->pauseSimulation();
			commitDocumentEdit(std::move(before), history);
		};
		auto verify = [&](std::vector<float> expected)
		{
			std::vector<float> actual;
			for (uint32_t i = 0; i < world->getSector(room)->getNumObjects(); ++i)
			{
				auto object = world->getSector(room)->getObject(i);
				auto button = object ? std::dynamic_pointer_cast<const core::Button>(object->_getObject()) : nullptr;
				if (!button) continue;
				actual.push_back(button->getPosition().x + button->getSize().x * 0.5f);
				require(world->lookupInteractionPoint(button->getInteractionPointId()).entity->getPosition()
					== core::Vector2{actual.back(), 1}, "Bridge history elevated or relocated approach");
			}
			std::sort(actual.begin(), actual.end()); require(actual == expected, "Bridge editor/history changed endpoint demand/insets");
		};
		core::World::CreateForceBridgeResult created;
		core::World::CreateForceBridgeOptions options{2, CORE_SIDE_RIGHT, true, false, 1};
		edit([&] { created = world->addSectorForceBridge(room, 1, 2, options); }); verify({4.25f});
		// Supported Selection configuration edits reconstruct exactly the requested
		// endpoints, not previous Button placement or the extension origin.
		options.controlCount = 2;
		edit([&] { world->applySectorForceBridgeOptions(room, created.forceBridge.index, options); }); verify({1.75f, 4.25f});
		// Clipboard copy/paste mirror: the production generic UI reads the authored
		// options and passes them to the same World preflight/add API.
		core::World::CreateForceBridgeOptions copied;
		require(world->getSectorForceBridgeOptions(room, created.forceBridge.index, copied), "Clipboard source lost authored Bridge options");
		edit([&] { world->addSectorForceBridge(room, 1, 6, copied); }); verify({1.75f, 4.25f, 5.75f, 8.25f});
		auto before = captureDocumentSnapshot(world, history); auto graph = world->getGraph(); bool refused = false;
		try { world->addSectorForceBridge(room, 1, 6, copied); } catch (std::exception const&) { refused = true; }
		require(refused && world->getGraph() == graph && captureDocumentSnapshot(world, history)->yaml == before->yaml
			&& history.undoCount() == 3, "Refused Bridge paste changed document/history");
		auto restore = [&](DocumentSnapshot const& snapshot)
		{
			world = deserializeDocumentSnapshot(snapshot, world, {}); world->pauseSimulation(); return bool(world);
		};
		for (auto expected : {std::vector<float>{1.75f, 4.25f}, {4.25f}, {}})
		{
			require(history.undo(*captureDocumentSnapshot(world, history), restore), "Bridge document undo failed"); verify(expected);
		}
		for (auto expected : {std::vector<float>{4.25f}, {1.75f, 4.25f}, {1.75f, 4.25f, 5.75f, 8.25f}})
		{
			require(history.redo(*captureDocumentSnapshot(world, history), restore), "Bridge document redo failed"); verify(expected);
		}
	}

	void controlsAndHistory()
	{
		using namespace broken_extensible;
		using smoke::require;
		bridgeInsetHistory();
		for (auto kind : { Kind::RoomLadder, Kind::TransitLadder, Kind::Bridge })
		{
			Scene scene(kind);
			auto world = scene.world;
			auto device = scene.device;
			world->pauseSimulation();
			gWorldDocumentHistory.clear(); gWorldDocumentHistory.markSaved(); world->markSaved();
			auto save = [&]
			{
				core::SerializationWorkData work; work.markSerializedUnmodified = false;
				auto writer = core::YamlSerializer::toString(); world->serialize(*writer, work); writer->serialize();
				return writer->getSerializedString();
			};
			auto initial = save();
			auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.LogFilename = nullptr;
			io.DisplaySize = ImVec2(1600, 1000);
			std::string text;
			io.ClipboardUserData = &text;
			io.SetClipboardTextFn = [](void* data, char const* value) { *static_cast<std::string*>(data) = value; };
			auto frame = [&]
			{
				ImGui::NewFrame();
				ImGui::SetNextWindowPos(ImVec2(10, 10)); ImGui::SetNextWindowSize(ImVec2(1500, 950));
				ImGui::Begin("Extensible Selection", nullptr, ImGuiWindowFlags_NoSavedSettings);
				auto depth = GImGui->DisabledStackSize; auto flags = GImGui->CurrentItemFlags; auto alpha = GImGui->Style.Alpha;
				text.clear(); ImGui::LogToClipboard();
				renderExtensibleConditionPanel(world, device, scene.resource);
				ImGui::LogFinish();
				require(depth == GImGui->DisabledStackSize && flags == GImGui->CurrentItemFlags && alpha == GImGui->Style.Alpha,
					"Extensible condition panel leaked disabled state");
				ImGui::End(); ImGui::Render();
			};
			auto click = [&](char const* label)
			{
				frame(); auto* window = ImGui::FindWindowByName("Extensible Selection"); auto control = window->GetID(label);
				bool found = false; ImVec2 point;
				for (float y = window->Pos.y + 25; y < window->Pos.y + 220 && !found; y += 8)
					for (float x = window->Pos.x + 5; x < window->Pos.x + 180 && !found; x += 16)
					{
						io.AddMousePosEvent(x, y); frame(); frame();
						if (ImGui::GetHoveredID() == control) { found = true; point = ImVec2(x, y); }
					}
				require(found, std::string("Control not reachable: ") + label);
				io.AddMousePosEvent(point.x, point.y); frame();
				io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); frame();
				io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); frame();
			};
			click("Initially Broken"); frame();
			require(device->isInitiallyBroken() && device->isBroken() && gWorldDocumentHistory.undoCount() == 1,
				"Authored control did not record one edit");
			require(text.find("Broken (position frozen)") != std::string::npos && text.find("100.00%") != std::string::npos,
				"Status omitted frozen physical extension");
			auto authored = save(); world->markSaved();
			click("Live Broken");
			require(!device->isBroken() && device->isInitiallyBroken() && save() == authored && !world->isModified()
				&& gWorldDocumentHistory.undoCount() == 1, "Live control overwrote document/history");
			require(world->resumeSimulation(), "Resume failed"); click("Live Broken");
			require(device->isBroken() && save() == authored && !world->isModified(), "Running live control failed");
			world->pauseSimulation();
			auto restore = [&](DocumentSnapshot const& snapshot)
			{
				auto loaded = std::make_shared<core::World>("History", 1, 1);
				core::SerializationWorkData work; auto reader = core::YamlSerializer::fromString(snapshot.yaml); reader->deserialize();
				require(loaded->deserialize(*reader, work), "Extensible history did not load"); world = loaded; world->pauseSimulation();
				return true;
			};
			require(gWorldDocumentHistory.undo(gWorldDocumentHistory.capture(save()), restore) && save() == initial, "Initial Broken undo failed");
			require(gWorldDocumentHistory.redo(gWorldDocumentHistory.capture(save()), restore) && save() == authored, "Initial Broken redo failed");

			Scene excluded(kind, true, false); world = excluded.world; device = excluded.device;
			frame();
			require(text.find("Broken") == std::string::npos, "Non-extensible device exposed Broken controls");
		}
	}
}

void editor_smoke::registerBrokenExtensibles(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "extensibles/controlsAndHistory", [](smoke::Context const&)
		{ State state; headless::ScopedImGuiContext context;
		ImGui::GetIO().Fonts->AddFontDefault(); ImGui::GetIO().Fonts->Build(); controlsAndHistory(); } });
}
