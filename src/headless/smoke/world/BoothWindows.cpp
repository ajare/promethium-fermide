#include "Checks.h"
#include "core/World.h"
#include "core/Graph.h"
#include "core/Pathing.h"
#include "core/WindowVertex.h"
#include "core/YamlSerializer.h"
#include <cmath>

namespace
{
	using smoke::require;
	std::string saved(core::World const& world)
	{
		auto writer = core::YamlSerializer::toString(); core::SerializationWorkData work;
		work.markSerializedUnmodified = false; world.serialize(*writer, work); writer->serialize();
		return writer->getSerializedString();
	}
	void placement(smoke::Context const&)
	{
		for (uint32_t front : {0u, 1u, 2u}) for (unsigned a = 0; a < 3; ++a) for (unsigned b = 0; b < 3; ++b)
			for (auto state : {core::Window::State::Closed, core::Window::State::Open})
			{
				core::World world("BoothWindow pairs", 8, 3); world.addLayer(); world.addLayer();
				auto location = [&](uint32_t layer, unsigned kind) {
					if (kind == 0) return world.addRoom("Room", layer, 0, 0, 7, 2);
					if (kind == 1) return world.addCorridor(layer, 0, 0, 7, 1);
					return world.addFacade(layer, 0, 0, 7, 2);
				};
				auto left = location(front, a), right = location(front + 1, b);
				auto created = world.addBoothWindow(front, 0, 3, state); world.finishBuild();
				require(created.window.type == core::SectorObjectType::BoothWindow && created.object->isBoothWindow()
					&& created.object->getState() == state && !created.object->isTraversalConfigured()
					&& !created.traversalResource && world.getSimulationSnapshot().traversalResources.empty()
					&& world.getSimulationSnapshot().interactionPoints.size() == 1, "BoothWindow leaked traversal or duplicated panel resources");
				auto booth = std::static_pointer_cast<const core::BoothWindow>(created.object);
				auto panel = world.lookupInteractionPoint(booth->getPanel()).entity;
				require(panel && panel->getSector() == core::SectorId{static_cast<uint64_t>(right) + 1}
					&& panel->getPosition().x == 3.5f && panel->getPosition().y == 0
					&& panel->getDurationTicks() == 1 && panel->getReach() == 0.25f,
					"Panel not at centred back-side walkable approach");
				auto position = created.object->getPosition(); auto size = created.object->getSize();
				require(std::abs(position.x - 3.1f) < 0.001f && std::abs(position.y - 0.2f) < 0.001f
					&& std::abs(size.x - 0.8f) < 0.001f && std::abs(size.y - 0.3f) < 0.001f, "Wrong aperture geometry");
				unsigned count = 0;
				for (auto const& vertex : world.getGraph()->getVertices())
					if (auto point = std::dynamic_pointer_cast<const core::WindowVertex>(vertex); point && point->getWindow() == created.object)
					{
						++count; auto p = point->getPosition();
						require(std::abs(p.x - 3.5f) < 0.001f && std::abs(p.y) < 0.001f, "Approach is not centred on walking Level");
					}
				require(count == 2, "BoothWindow must have exactly two approaches");
				world.pauseSimulation(); uint32_t from, to;
				world.addSectorMarker(left, 0, 0.5f, &from); world.addSectorMarker(right, 0, 0.5f, &to); world.finishBuild();
				auto graph = world.getGraph();
				require(!core::pathing::findPath(nullptr, graph.get(), graph->getVertexByIdentifier(from), graph->getVertexByIdentifier(to)), "BoothWindow allowed crossing");
				for (auto const& vertex : graph->getVertices())
					if (auto point = std::dynamic_pointer_cast<const core::WindowVertex>(vertex); point && point->getWindow() == created.object)
						require(bool(core::pathing::findPath(nullptr, graph.get(), graph->getVertexByIdentifier(point->getSector()->getIndex() == left ? from : to), point)), "Approach disconnected from same-Layer walking topology");
				world.addSectorWindow(front, 0, 5, 1, 1, {true, core::Window::State::Open, core::Window::Style::Clear}); world.finishBuild(); graph = world.getGraph();
				require(bool(core::pathing::findPath(nullptr, graph.get(), graph->getVertexByIdentifier(from), graph->getVertexByIdentifier(to))), "Ordinary Window positive crossing control failed");
			}
	}
	void simultaneousPairs(smoke::Context const&)
	{
		core::World world("Stacked BoothWindows",8,2); world.addLayer(); world.addLayer();
		for (uint32_t layer=0;layer<4;++layer) world.addRoom("Room",layer,0,0,7,1);
		for (uint32_t layer=0;layer<3;++layer) world.addBoothWindow(layer,0,3,
			layer==1 ? core::Window::State::Open : core::Window::State::Closed);
		world.finishBuild();
		std::array<unsigned,4> counts{};
		for (auto const& vertex : world.getGraph()->getVertices())
			if (auto point=std::dynamic_pointer_cast<const core::WindowVertex>(vertex); point && point->getWindow()->isBoothWindow())
				++counts[point->getSector()->getLayerIndex()];
		require(counts==std::array<unsigned,4>{1,2,2,1},"Overlapping adjacent pairs lost an approach or duplicated one");
		for (uint32_t from=0;from<4;++from) for (uint32_t to=from+1;to<4;++to)
		{
			auto a=world.getGraph()->getVertexAtPosition(from,3.5f,0,0.01f);
			auto b=world.getGraph()->getVertexAtPosition(to,3.5f,0,0.01f);
			require(a && b && !core::pathing::findPath(nullptr,world.getGraph().get(),a,b),"Stacked BoothWindows paired across Layers");
		}
	}

	void lifecycle(smoke::Context const&)
	{
		for (unsigned operation=0;operation<6;++operation)
		{
			core::World world("BoothWindow lifecycle",10,4); world.addLayer(); world.addLayer();
			world.addRoom("Front",1,0,0,8,3); world.addRoom("Back",2,0,0,8,3);
			world.addSectorWalkway(0,1,3); world.addSectorWalkway(1,1,3);
			world.addBoothWindow(1,1,3,core::Window::State::Open); world.finishBuild(); world.pauseSimulation();
			if (operation==0) world.applyLocationEdit(world.planResizeLocation(1,0,0,9,3));
			if (operation==1) world.applyLocationEdit(world.planResizeLocation(1,0,0,3,3));
			if (operation==2) world.applyLocationEdit(world.planRemoveLocation(1));
			if (operation==3) world.applyDeleteLayer(world.planDeleteLayer(0));
			if (operation==4) world.applyDeleteLayer(world.planDeleteLayer(2));
			if (operation==5)
			{
				try { world.removeSectorWalkway(1,0); }
				catch (std::exception const& error) { throw smoke::Failure(std::string("Walkway support removal: ")+error.what()); }
			}
			unsigned approaches=0;
			for (auto const& vertex : world.getGraph()->getVertices())
				if (auto point=std::dynamic_pointer_cast<const core::WindowVertex>(vertex); point && point->getWindow()->isBoothWindow())
				{
					++approaches;
					require(point->getWindow()->getBackLayer()==point->getWindow()->getFrontLayer()+1,"Lifecycle broke adjacent pair");
				}
			require(world.getSimulationSnapshot().interactionPoints.size() == ((operation==0 || operation==3) ? 1u : 0u),
				"Lifecycle retained invalid panel or lost reconstructed panel");
			for (auto const& panel : world.getSimulationSnapshot().interactionPoints)
				require(panel.position.x == 3.5f && panel.position.y == 1,
					"Reconstructed panel lost walkable approach position");
			require(approaches==((operation==0 || operation==3) ? 2u : 0u),"Lifecycle retained invalid BoothWindow/stale approach or lost valid pair");
		}
		core::World transit("BoothWindow Transit refusal",10,4);
		transit.addRoom("Front",0,0,0,8,3); transit.addCorridor(0,3,0,8,1);
		transit.addLadder(1,0,3,{4,false,true}); transit.finishBuild(); transit.pauseSimulation(); transit.markSaved();
		auto baseline=saved(transit); std::string diagnostic;
		require(!transit.canAddBoothWindow(0,0,3,1,1,&diagnostic) && !diagnostic.empty(),"Transit BoothWindow accepted");
		bool refused=false; try { transit.addBoothWindow(0,0,3); } catch (std::exception const&) { refused=true; }
		require(refused && saved(transit)==baseline && !transit.isModified() && transit.isTraversalTopologyValid(),"Transit refusal mutated World");
	}

	void refusal(smoke::Context const&)
	{
		core::World world("BoothWindow refusal", 10, 3);
		world.addRoom("Front", 0, 0, 0, 5, 2); world.addRoom("Back", 1, 0, 0, 5, 2);
		world.addBackground(1, 0, 5, 2, 1); world.addRoom("Front background", 0, 0, 5, 2, 1);
		world.finishBuild(); world.pauseSimulation(); world.markSaved(); auto baseline = saved(world);
		for (auto p : {std::array<uint32_t,5>{1,0,1,1,1}, {0,0,8,1,1}, {0,0,5,1,1}, {0,1,1,1,1}, {0,0,1,0,1}, {0,0,1,2,1}, {0,0,1,1,2}})
		{
			std::string diagnostic; require(!world.canAddBoothWindow(p[0],p[1],p[2],p[3],p[4], &diagnostic) && !diagnostic.empty(), "Invalid placement accepted");
			bool refused = p[3]!=1 || p[4]!=1;
			if (!refused) try { world.addBoothWindow(p[0],p[1],p[2]); } catch (std::exception const&) { refused = true; }
			require(refused && saved(world) == baseline && !world.isModified() && world.isTraversalTopologyValid(), "Rejected placement mutated World");
		}
		auto booth = world.addBoothWindow(0,0,1).object; world.finishBuild(); world.markSaved(); baseline = saved(world);
		for (unsigned operation = 0; operation < 4; ++operation)
		{
			bool refused = false; try {
				if (operation == 0) booth->setState(core::Window::State::Broken);
				if (operation == 1) booth->setState(core::Window::State::Open, core::Window::Style::Tinted);
				if (operation == 2) booth->configureTraversal(true, {});
				if (operation == 3) world.createWindowTraversalResource("Forbidden", booth);
			} catch (std::exception const&) { refused = true; }
			require(refused && booth->getState() == core::Window::State::Closed && !booth->isTraversalConfigured()
				&& saved(world) == baseline && !world.isModified() && world.isTraversalTopologyValid(), "Base API leaked unsupported capabilities");
		}
	}
}
void registerBoothWindows(std::vector<smoke::Check>& checks)
{
	checks.push_back({"boothWindows/placementAndTopology", placement});
	checks.push_back({"boothWindows/atomicRefusal", refusal});
	checks.push_back({"boothWindows/lifecycle", lifecycle});
	checks.push_back({"boothWindows/simultaneousAdjacentPairs", simultaneousPairs});
}
