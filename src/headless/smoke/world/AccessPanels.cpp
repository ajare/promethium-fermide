#include "Checks.h"
#include "core/World.h"
#include "core/Pathing.h"
#include "core/YamlSerializer.h"
#include <limits>

namespace
{
	using smoke::require;
	std::string saved(core::World const& world)
	{
		auto out = core::YamlSerializer::toString(); core::SerializationWorkData work;
		work.markSerializedUnmodified = false; world.serialize(*out, work); out->serialize(); return out->getSerializedString();
	}
	void authored(smoke::Context const&)
	{
		for (unsigned kind = 0; kind < 3; ++kind)
		{
			core::World world("Panels", 10, 4);
			auto owner = kind == 0 ? world.addRoom("Room", 0, 1, 1, 7, 2)
				: kind == 1 ? world.addCorridor(0, 1, 1, 7, 1) : world.addFacade(0, 1, 1, 7, 2);
			auto other = world.addRoom("Back", 1, 1, 1, 7, 2);
			world.addSectorMarker(other, 0, 0.5f);
			if (kind != 1) world.addSectorWalkway(owner, 1, 3);
			uint32_t marker; world.addSectorMarker(owner, 0, 0.5f, &marker);
			world.finishBuild(); world.pauseSimulation(); world.markSaved();
			auto baseline = saved(world); auto graph = world.getGraph();
			for (float invalid : {-0.01f, 1.01f, std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
				for (unsigned field = 0; field < 3; ++field)
				{
					core::AccessPanelGeometry g; if (field == 0) g.width = invalid; if (field == 1) g.height = invalid; if (field == 2) g.yOffset = invalid;
					std::string reason; require(!world.canAddAccessPanel(owner, 0, 3, g, &reason) && !reason.empty(), "Invalid geometry accepted");
					bool refused = false; try { world.addAccessPanel(owner, 0, 3, g); } catch (std::exception const&) { refused = true; }
					require(refused && saved(world) == baseline && world.getGraph() == graph && !world.isModified(), "Geometry refusal mutated World/graph");
				}
			for (auto cell : {std::array<uint32_t,3>{owner,0,0}, {owner,0,8}, {owner,2,3}, {99,0,3}})
				require(!world.canAddAccessPanel(cell[0],cell[1],cell[2]), "Invalid cell accepted");
			require(!world.canAddAccessPanel(owner,0,3,{1,0.75f,0.5f}), "Invalid vertical sum accepted");
			if (kind != 1) require(!world.canAddAccessPanel(owner,1,2), "Unsupported air cell accepted");
			auto created = world.addAccessPanel(owner,0,3); world.finishBuild();
			auto object = created.sector->getObject(created.index);
			auto panel = std::dynamic_pointer_cast<const core::AccessPanel>(object->_getObject());
			require(panel && panel->getPosition() == core::Vector2{3.25f,1.25f} && panel->getSize() == core::Vector2{0.5f,0.25f}
				&& panel->getType() == core::AccessPanel::Type::Empty && panel->getState() == core::AccessPanel::State::Closed, "Wrong default panel");
			require(!world.canAddAccessPanel(owner,0,3,{0,0,1}), "Cell uniqueness ignores degenerate panels");
			auto vertex = world.getGraph()->getVertexForObject(object);
			require(vertex && vertex->getPosition() == core::Vector2{3.5f,1}
				&& core::pathing::findPath(nullptr,world.getGraph().get(),world.getGraph()->getVertexByIdentifier(marker),vertex), "Approach disconnected or not cell-centred/floor-Level");
			auto back = world.getGraph()->getClosestVertexInSector(world.getSector(other).get(), {1.5f,1});
			require(!core::pathing::findPath(nullptr,world.getGraph().get(),vertex,back) && world.getSimulationSnapshot().traversalResources.empty(), "Panel creates a passage/resource");
			world.markSaved(); baseline = saved(world); graph = world.getGraph();
			require(!world.configureAccessPanel(owner,created.index,{0.5f,1,0.01f}) && saved(world) == baseline && world.getGraph() == graph && !world.isModified(), "Rejected edit mutated authored state");
			for (float invalid : {-0.01f,1.01f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
				for (unsigned field=0;field<3;++field)
				{
					core::AccessPanelGeometry g; if (field==0) g.width=invalid; if (field==1) g.height=invalid; if (field==2) g.yOffset=invalid;
					require(!world.configureAccessPanel(owner,created.index,g) && saved(world)==baseline
						&& world.getGraph()==graph && !world.isModified(), "Invalid property edit changed World/graph");
				}
			for (auto g : {core::AccessPanelGeometry{1,1,0}, {0,0,1}, {0,1,0}, {1,0,1}, {1,0.75f,0.25f}})
			{
				require(world.configureAccessPanel(owner,created.index,g), "Inclusive geometry boundary refused");
				require(panel->getGeometry() == g && world.getGraph() == graph, "Geometry changes approach topology");
				require(!world.canAddAccessPanel(owner,0,3,{0,0,0}), "Degenerate panel lost cell ownership uniqueness");
				core::Vector2 min,max; panel->getSelectionShape(min,max); auto centre=(min+max)*0.5f;
				std::shared_ptr<const core::SectorObject> selected;
				require(world.getObjectAtPosition(0,centre.x,centre.y,&selected) == panel && selected == object, "Panel/degenerate indicator not selectable");
			}
			if (kind != 1)
			{
				auto upper=world.addAccessPanel(owner,1,4); world.finishBuild();
				require(world.getGraph()->getVertexForObject(upper.sector->getObject(upper.index))->getPosition() == core::Vector2{4.5f,2}, "Walkway approach wrong Level");
			}
			require(world.removeAccessPanel(owner,created.index)
				&& (created.index >= created.sector->getNumObjects() || !created.sector->getObject(created.index))
				&& !world.getGraph()->getVertexForObject(object) && world.canAddAccessPanel(owner,0,3), "Deletion left object/vertex/cell orphan");
			if (kind == 2) for (uint32_t level=0;level<2;++level) for (int side : {0,1})
				require(world.getSector(owner)->getEndType(level,side)==core::SectorEndType::None, "Panel created Facade wall");
		}
		{
			core::World bridge("Bridge support",8,3); auto room=bridge.addRoom("Room",0,0,0,7,2);
			bridge.addSectorWalkway(room,1,1); bridge.addSectorWalkway(room,1,3);
			bridge.addSectorForceBridge(room,1,2,{1,CORE_SIDE_LEFT,true,true,1}); bridge.finishBuild(); bridge.pauseSimulation();
			require(!bridge.canAddAccessPanel(room,1,2), "Force Bridge accepted as panel support");
		}
		core::World unsupported("Unsupported",8,4);
		auto background=unsupported.addBackground(0,0,0,2,2);
		require(!unsupported.canAddAccessPanel(background,0,0), "Background admitted panel");
		unsupported.addCorridor(0,0,3,2,1); unsupported.addCorridor(0,3,3,2,1);
		auto ladder=unsupported.addLadder(1,0,3,{4,false,true});
		require(!unsupported.canAddAccessPanel(ladder.ladder.sector->getIndex(),0,3), "Transit admitted panel");
	}
	void overlap(smoke::Context const& context)
	{
		for (unsigned kind=0;kind<4;++kind)
		{
			core::World world("Overlap",8,2); auto room=world.addRoom("Front",0,0,0,7,1);
			world.addRoom("Back",1,0,0,7,1);
			if (kind==0) world.addSectorDoor(0,0,3);
			if (kind==1) world.addSectorWindow(0,0,3,1,1);
			if (kind==2) world.addBoothWindow(0,0,3);
			if (kind==3) world.addSectorLightSwitch(room,3);
			world.finishBuild(); world.pauseSimulation(); auto before=saved(world); auto graph=world.getGraph();
			require(!world.canAddAccessPanel(room,0,3,{1,1,0}), "Panel overlaps fixed wall object");
			bool rejected=false; try { world.addAccessPanel(room,0,3,{1,1,0}); } catch (std::exception const&) { rejected=true; }
			require(rejected && saved(world)==before && world.getGraph()==graph, "Overlap rejection mutated state");
			auto created=world.addAccessPanel(room,0,3,{0,0,0}); world.finishBuild();
			require(!world.configureAccessPanel(room,created.index,{1,1,0}), "Panel expansion overlaps fixed object");
		}
		for (unsigned kind=0;kind<3;++kind) for (bool routed : {false,true})
		{
			core::World furnished("Furniture overlap",8,2);
			auto room=kind==0 ? furnished.addRoom("Room",0,0,0,7,1) : kind==1 ? furnished.addCorridor(0,0,0,7,1) : furnished.addFacade(0,0,0,7,1);
			auto filename=routed ? "furniture.furniture.yaml" : "chair.furniture.yaml";
			furnished.attachFurnitureCatalogue(filename,core::FurnitureCatalogue::readFile(context.fixture(std::string("resources/test-worlds/")+filename)));
			furnished.placeFurniture(room,routed ? "desk" : "chair",3,0,"Furniture");
			uint32_t from,to; furnished.addSectorMarker(room,0,1.5f,&from); furnished.addSectorMarker(room,0,6.5f,&to);
			auto panel=furnished.addAccessPanel(room,0,3,{1,1,0}); furnished.finishBuild();
			auto graph=furnished.getGraph(); auto vertex=graph->getVertexForObject(panel.sector->getObject(panel.index));
			require(furnished.furniture().size()==1 && vertex
				&& core::pathing::findPath(nullptr,graph.get(),graph->getVertexByIdentifier(from),vertex)
				&& core::pathing::findPath(nullptr,graph.get(),graph->getVertexByIdentifier(to),vertex), "Furniture overlap stranded panel approach");
			if (routed) require(vertex->getEdges().size()==1, "Panel created a shortcut through Furniture routes");
		}
		{
			core::World edges("Wall edge contact",8,2); auto room=edges.addRoom("Room",0,0,0,7,1);
			edges.addRoom("Back",1,0,0,7,1); edges.addBoothWindow(0,0,3);
			require(edges.canAddAccessPanel(room,0,3,{0.5f,0.25f,0.5f}), "Fixed wall edge contact refused");
		}
		core::World world("Edges",6,2); auto room=world.addRoom("Room",0,0,0,5,1);
		world.addAccessPanel(room,0,1,{1,1,0}); require(world.canAddAccessPanel(room,0,2,{1,1,0}), "Panel edge contact rejected");
		world.addAccessPanel(room,0,2,{1,1,0});
		world.finishBuild(); world.pauseSimulation(); world.createAgent("In front",room,0,3.5f);
		require(world.canAddAccessPanel(room,0,3), "Agent presence rejects panel");
	}
}
void registerAccessPanels(std::vector<smoke::Check>& checks)
{
	checks.push_back({"accessPanels/authoredWorld",authored});
	checks.push_back({"accessPanels/wallOverlap",overlap});
}
