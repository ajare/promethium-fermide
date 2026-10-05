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
	void movement()
	{
		for (auto geometry : {core::AccessPanelGeometry{}, {0,0.25f,0.25f}, {0.5f,0,1}, {0,0,0}})
		{
			core::World world("Panel moves",16,4);
			auto room = world.addRoom("Room",0,0,0,5,3);
			auto corridor = world.addCorridor(0,0,6,4,1);
			auto facade = world.addFacade(0,0,11,4,3);
			auto background = world.addBackground(0,3,6,4,1);
			(void)background;
			world.addSectorWalkway(room,1,2); world.addSectorWalkway(facade,1,2);
			auto created = world.addAccessPanel(room,0,1,geometry);
			world.addAccessPanel(corridor,0,7,{0,0,0});
			world.finishBuild(); world.pauseSimulation(); world.markSaved();
			auto before = saved(world); auto graph = world.getGraph();
			for (auto target : {core::Vector2{7,0}, {6,3}, {3,1}, {16,0}, {0,4}})
			{
				auto plan = world.planMoveSectorObject(room,created.index,uint32_t(target.x),uint32_t(target.y));
				require(!plan.valid, "Invalid panel move preview accepted");
				bool rejected = false; try { world.applyObjectMove(plan); } catch (std::exception const&) { rejected = true; }
				require(rejected && saved(world)==before && world.getGraph()==graph && !world.isModified(), "Invalid move partially changed ownership/graph");
			}
			std::shared_ptr<const core::SectorObject> object = created.sector->getObject(created.index);
			for (auto target : {core::Vector2{8,0}, {13,1}, {2,1}, {12,0}})
			{
				auto old = object; auto oldPanel = std::static_pointer_cast<const core::AccessPanelSectorObject>(old)->getPanel();
				auto source = old->getSector()->getIndex(); uint32_t index=0;
				while (world.getSector(source)->getObject(index)!=old) ++index;
				auto control = oldPanel->getControl(core::AccessPanel::Action::Open);
				auto plan = world.planMoveSectorObject(source,index,uint32_t(target.x),uint32_t(target.y));
				require(plan.valid, "Valid Location panel move refused: " + plan.diagnostic);
				object = world.applyObjectMove(plan);
				auto panel = std::static_pointer_cast<const core::AccessPanelSectorObject>(object)->getPanel();
				auto vertex = world.getGraph()->getVertexForObject(std::const_pointer_cast<core::SectorObject>(object));
				require(panel->getGeometry()==geometry && panel->getType()==core::AccessPanel::Type::Empty
					&& panel->getState()==core::AccessPanel::State::Closed && vertex
					&& vertex->getPosition()==core::Vector2{target.x+0.5f,target.y}
					&& !world.getGraph()->getVertexForObject(std::const_pointer_cast<core::SectorObject>(old))
					&& !world.lookupInteractionPoint(control) && !world.lookupAccessPanel(oldPanel->getId()), "Move retained stale owned references");
				require(world.canAddAccessPanel(source,oldPanel->getLevelOffset(),old->getCellX(),geometry), "Move left source attachment");
				core::Vector2 min,max; panel->getSelectionShape(min,max); auto centre=(min+max)*0.5f;
				std::shared_ptr<const core::SectorObject> selected;
				require(world.getObjectAtPosition(0,centre.x,centre.y,&selected)==panel && selected==object, "Moved degenerate panel selection stale");
			}
		}
		core::World conflict("Move wall conflict",8,2); auto room=conflict.addRoom("Room",0,0,0,7,1);
		conflict.addRoom("Back",1,0,0,7,1); conflict.addBoothWindow(0,0,3);
		auto created=conflict.addAccessPanel(room,0,1,{1,1,0}); conflict.finishBuild(); conflict.pauseSimulation(); conflict.markSaved();
		auto before=saved(conflict); auto graph=conflict.getGraph();
		require(!conflict.planMoveSectorObject(room,created.index,3,0).valid && saved(conflict)==before
			&& conflict.getGraph()==graph && !conflict.isModified(), "Move wall overlap mutated document");
	}
	std::shared_ptr<const core::AccessPanelSectorObject> panelIn(core::World const& world, uint32_t sector)
	{
		for (uint32_t i = 0; i < world.getSector(sector)->getNumObjects(); ++i)
			if (auto p = std::dynamic_pointer_cast<const core::AccessPanelSectorObject>(world.getSector(sector)->getObject(i))) return p;
		return {};
	}
	void structural()
	{
		for (unsigned kind = 0; kind < 3; ++kind)
			for (auto geometry : {core::AccessPanelGeometry{}, {0,0,1}, {1,1,0}})
			{
				core::World world("Carried panels", 16, 5);
				auto owner = kind == 0 ? world.addRoom("Room",0,1,1,5,2)
					: kind == 1 ? world.addCorridor(0,1,1,5,1) : world.addFacade(0,1,1,5,2);
				if (kind != 1) world.addSectorWalkway(owner,1,3);
				auto level = kind == 1 ? 0u : 1u;
				auto created = world.addAccessPanel(owner,level,4,geometry);
				world.finishBuild(); world.pauseSimulation();
				auto actor = world.createAgent("Pending",owner,level,3.5f);
				auto old = panelIn(world,owner); auto panel = old->getPanel();
				auto point = panel->getControl(core::AccessPanel::Action::Open);
				auto pending = world.requestAccessPanel(panel->getId(),core::AccessPanel::Action::Open,actor);
				require(bool(pending), "Structural pending request refused");
				auto requestState=world.lookupInteractionRequest(pending).entity;
				auto operation=requestState->getOperations().front().first;
				auto operationState=world.lookupDeviceOperation(operation).entity;
				world.markSaved(); auto before = saved(world); auto graph = world.getGraph();
				auto resize = [&](uint32_t width,uint32_t height) { return kind == 2
					? world.planResizeFacade(owner,1,1,width,height) : world.planResizeLocation(owner,1,1,width,height); };
				for (auto plan : {resize(3,kind == 1 ? 1 : 2), resize(5,level)})
				{
					require(!plan.valid, "Location shrink discarded retained panel");
					bool refused = false; try { world.applyLocationEdit(plan); } catch (std::exception const&) { refused = true; }
					require(refused && saved(world)==before && world.getGraph()==graph && !world.isModified()
						&& world.lookupInteractionRequest(pending).entity==requestState
						&& world.lookupDeviceOperation(operation).entity==operationState && world.lookupInteractionPoint(point), "Resize preflight mutated live state");
				}
				if (kind != 1)
				{
					auto walkway = std::as_const(world).getLayer(0)->getCellDefinition(4,2).floorIndex;
					require(!world.planRemoveSectorWalkway(owner,walkway).valid, "Walkway preview removed panel support");
					bool refused = false; try { world.removeSectorWalkway(owner,walkway); } catch (std::exception const&) { refused = true; }
					require(refused && saved(world)==before && world.getGraph()==graph && !world.isModified()
						&& world.lookupInteractionRequest(pending).entity, "Walkway support removal was not atomic");
				}
				auto move = kind == 2 ? world.planResizeFacade(owner,7,2,5,2)
					: world.planResizeLocation(owner,7,2,5,kind == 1 ? 1 : 2);
				require(move.valid, "Location carrying panel refused: " + move.diagnostic);
				owner = world.applyLocationEdit(move);
				auto carried = panelIn(world,owner); auto vertex = world.getGraph()->getVertexForObject(std::const_pointer_cast<core::AccessPanelSectorObject>(carried));
				require(carried && carried->getCellX()==10 && carried->getCellY()==2+level
					&& carried->getPanel()->getGeometry()==geometry && carried->getPanel()->getLevelOffset()==level
					&& vertex && vertex->getPosition()==core::Vector2{10.5f,float(2+level)}
					&& world.lookupInteractionPoint(carried->getPanel()->getControl(core::AccessPanel::Action::Open)).entity->getPosition()==vertex->getPosition()
					&& !world.lookupInteractionPoint(point) && !world.lookupInteractionRequest(pending).entity
					&& !world.lookupDeviceOperation(operation) && !world.lookupAccessPanel(panel->getId()) && !world.getGraph()->getVertexForObject(std::const_pointer_cast<core::AccessPanelSectorObject>(old)), "Location move retained stale panel geometry/handles");
				actor = world.createAgent("Removal pending",owner,level,3.5f);
				panel = carried->getPanel(); point = panel->getControl(core::AccessPanel::Action::Open);
				pending = world.requestAccessPanel(panel->getId(),core::AccessPanel::Action::Open,actor);
				require(bool(pending), "Carried panel interactions unusable");
				operation=world.lookupInteractionRequest(pending).entity->getOperations().front().first;
				auto remove = kind == 2 ? world.planRemoveFacade(owner) : world.planRemoveLocation(owner);
				require(remove.valid, "Owning Location deletion refused"); world.applyLocationEdit(remove);
				require(!world.lookupAccessPanel(panel->getId()) && !world.lookupInteractionPoint(point)
					&& !world.lookupInteractionRequest(pending).entity && !world.lookupDeviceOperation(operation)
					&& !world.getGraph()->getVertexForObject(std::const_pointer_cast<core::AccessPanelSectorObject>(carried)), "Location deletion orphaned panel state");
				owner = world.addRoom("Replacement",0,2,7,5,2); world.addSectorWalkway(owner,1,3);
				created = world.addAccessPanel(owner,level,10,geometry); world.finishBuild();
				require(panelIn(world,owner)->getPanel()->getId()!=panel->getId()
					&& !world.requestAccessPanel(panel->getId(),core::AccessPanel::Action::Open,actor), "Replacement reused removed panel identity");
			}
		{
			core::World floor("Ground Floor removal",8,3); auto owner=floor.addRoom("Room",0,0,0,7,2);
			floor.addAccessPanel(owner,0,3,{0,0,0}); floor.finishBuild(); floor.pauseSimulation(); floor.markSaved();
			auto before=saved(floor); auto graph=floor.getGraph();
			require(!floor.planResizeLocation(owner,0,1,7,1).valid && saved(floor)==before
				&& floor.getGraph()==graph && !floor.isModified(), "Ground Floor removal relocated retained panel");
		}
		{
			core::World history("Retired panels",10,3); auto owner=history.addRoom("Room",0,0,0,8,2);
			auto walkway=history.addSectorWalkway(owner,1,6); auto removed=history.addAccessPanel(owner,1,6);
			auto retained=history.addAccessPanel(owner,0,2); history.finishBuild(); history.pauseSimulation();
			history.configureAccessPanel(owner,removed.index,{1,1,0}); history.removeAccessPanel(owner,removed.index);
			require(history.removeSectorWalkway(owner,walkway.index), "Removed historical panel blocked support removal");
			auto resize=history.planResizeLocation(owner,0,0,5,1);
			require(resize.valid, "Removed historical panel blocked shrink: "+resize.diagnostic); owner=history.applyLocationEdit(resize);
			require(history.getSector(owner)->getObject(retained.index) && panelIn(history,owner)->getCellX()==2,
				"Retiring panel chronology shifted surviving object indices");
		}
		for (bool layerEdit : {false,true})
		{
			core::World world("Layer/Level panels",8,4); world.addLayer(); world.addLayer();
			auto owner = world.addRoom("Retained",2,2,0,7,1); world.addAccessPanel(owner,0,3,{0,0,0});
			world.finishBuild(); world.pauseSimulation(); auto old = panelIn(world,owner)->getPanel();
			if (layerEdit) world.applyDeleteLayer(world.planDeleteLayer(0));
			else world.applyDeleteLevel(world.planDeleteLevel(0));
			auto p = panelIn(world,0);
			require(p && p->getSector()->getLayerIndex()==(layerEdit ? 1u : 2u) && p->getCellY()==(layerEdit ? 2u : 1u)
				&& p->getPanel()->getState()==core::AccessPanel::State::Closed && !world.lookupAccessPanel(old->getId()), "Layer/Level compaction lost valid panel");
			old = p->getPanel();
			if (layerEdit) world.applyDeleteLayer(world.planDeleteLayer(1));
			else world.applyDeleteLevel(world.planDeleteLevel(1));
			require(!world.lookupAccessPanel(old->getId()) && !world.lookupInteractionPoint(old->getControl(core::AccessPanel::Action::Open)), "Layer/Level owner deletion orphaned panel");
		}
	}
	void authored(smoke::Context const&)
	{
		movement();
		structural();
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
	void reverseOverlap()
	{
		for (unsigned kind=0; kind<5; ++kind) for (uint32_t panelLayer : {0u,1u})
		{
			core::World world("Reverse wall conflicts",10,2);
			auto front = world.addRoom("Front",0,0,0,9,1); auto back = world.addRoom("Back",1,0,0,9,1);
			auto owner = panelLayer == 0 ? front : back;
			world.addAccessPanel(owner,0,3,{1,1,0});
			uint32_t object=~0u;
			if (kind==0) object=world.addSectorDoor(0,0,5).door.index;
			if (kind==1) object=world.addSectorWindow(0,0,5,1,1);
			if (kind==2) object=world.addBoothWindow(0,0,5).window.index;
			world.finishBuild(); world.pauseSimulation(); world.markSaved(); auto before=saved(world); auto graph=world.getGraph();
			bool refused=false;
			try
			{
				if (kind==0) world.addSectorDoor(0,0,3);
				if (kind==1) world.addSectorWindow(0,0,3,1,1);
				if (kind==2) world.addBoothWindow(0,0,3);
				if (kind==3) world.addSectorLightSwitch(owner,3);
				if (kind==4) world.addAccessPanel(owner,0,3,{0,0,0});
			}
			catch (std::exception const&) { refused=true; }
			require(refused && saved(world)==before && world.getGraph()==graph && !world.isModified(), "Reverse wall placement was not atomic");
			if (object!=~0u)
			{
				require(!world.planMoveSectorObject(front,object,3,0).valid, "Wall object moved into panel");
				auto plan = kind==0 ? world.planResizeSectorDoor(front,object,3,0,2,1)
					: world.planResizeSectorWindow(front,object,3,0,3,1);
				require(!plan.valid && saved(world)==before && world.getGraph()==graph && !world.isModified(), "Wall object resize overlapped panel");
			}
		}
		// The first Button ends at .35. Adding a second Door stacks it at .375,
		// intersecting a panel on the preceding cell without aperture overlap.
		core::World stack("Reflow into panel",6,1); stack.addLayer();
		stack.addRoom("Front",0,0,0,6,1); auto middle=stack.addRoom("Middle",1,0,1,2,1); stack.addRoom("Back",2,0,0,6,1);
		stack.addSectorDoor(0,0,2,core::World::RemoteControlledDoor1Options);
		stack.addAccessPanel(middle,0,1,{1,0.1f,0.375f}); stack.finishBuild(); stack.pauseSimulation(); stack.markSaved();
		auto before=saved(stack); auto graph=stack.getGraph(); bool refused=false;
		try { stack.addSectorDoor(1,0,2,core::World::RemoteControlledDoor1Options); } catch (std::exception const&) { refused=true; }
		require(refused && saved(stack)==before && stack.getGraph()==graph && !stack.isModified(), "Final Button stack reflow partially committed");
		core::World support("Support reflow",8,3);
		auto room=support.addRoom("Room",0,0,0,7,2); support.addCorridor(1,1,0,7,1);
		for (uint32_t x : {2u,3u,4u}) support.addSectorWalkway(room,1,x);
		support.addSectorDoor(0,1,3,core::World::RemoteControlledDoor1Options);
		support.addAccessPanel(room,1,2,{1,0.1f,0.25f}); support.finishBuild(); support.pauseSimulation(); support.markSaved();
		before=saved(support); graph=support.getGraph();
		auto walkway=std::as_const(support).getLayer(0)->getCellDefinition(4,1).floorIndex;
		require(!support.planRemoveSectorWalkway(room,walkway).valid, "Support removal preview missed Button reflow overlap");
		refused=false; try { support.removeSectorWalkway(room,walkway); } catch (std::exception const&) { refused=true; }
		require(refused && saved(support)==before && support.getGraph()==graph && !support.isModified(), "Support-triggered Button reflow partially committed");
		// Degenerate bounds and touching edges remain valid in reverse order.
		for (auto geometry : {core::AccessPanelGeometry{0,1,0}, {1,0,0}, {0.5f,0.25f,0.5f}})
		{
			core::World edges("Reverse edges",8,2); auto host=edges.addRoom("Room",0,0,0,7,1); edges.addRoom("Back",1,0,0,7,1);
			edges.addAccessPanel(host,0,3,geometry); edges.addBoothWindow(0,0,3); edges.finishBuild();
			require(panelIn(edges,host)->getPanel()->getGeometry()==geometry, "Edge contact/zero area changed authored panel");
		}
		core::World height("Door height panel",8,2); auto host=height.addRoom("Room",0,0,0,7,1); height.addRoom("Back",1,0,0,7,1);
		height.addSectorDoor(0,0,3); height.addAccessPanel(host,0,3,{0.5f,0.1f,0.8f}); height.finishBuild(); height.pauseSimulation(); height.markSaved();
		before=saved(height); graph=height.getGraph(); std::string reason;
		require(!height.setSectorDoorHeight(0,0,3,1,core::Door::Height::Tall,&reason)
			&& saved(height)==before && height.getGraph()==graph && !height.isModified(), "Tall Door expansion was not atomic");
	}
	void overlap(smoke::Context const& context)
	{
		reverseOverlap();
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
			auto filename=routed ? "furniture.furniture.lua" : "chair.furniture.lua";
			furnished.attachFurnitureCatalogue(filename,core::FurnitureCatalogue::readFile(context.fixture(std::string("src/headless/smoke/fixtures/furniture/")+filename)));
			furnished.placeFurniture(room,routed ? "desk" : "chair",3,0,"Furniture");
			furnished.addSectorMarker(room,0,1.5f); furnished.addSectorMarker(room,0,6.5f);
			auto panel=furnished.addAccessPanel(room,0,1,{1,1,0}); furnished.finishBuild(); furnished.pauseSimulation();
			furnished.createAgent("In front",room,0,3.5f);
			auto moved=furnished.applyObjectMove(furnished.planMoveSectorObject(room,panel.index,3,0));
			auto graph=furnished.getGraph(); auto vertex=graph->getVertexForObject(std::const_pointer_cast<core::SectorObject>(moved));
			require(furnished.furniture().size()==1 && vertex
				&& core::pathing::findPath(nullptr,graph.get(),graph->getClosestVertexInSector(furnished.getSector(room).get(),{1.5f,0}),vertex)
				&& core::pathing::findPath(nullptr,graph.get(),graph->getClosestVertexInSector(furnished.getSector(room).get(),{6.5f,0}),vertex), "Furniture overlap stranded panel approach");
			if (routed) require(vertex->getEdges().size()==1, "Panel created a shortcut through Furniture routes");
		}
		{
			core::World edges("Wall edge contact",8,2); auto room=edges.addRoom("Room",0,0,0,7,1);
			edges.addRoom("Back",1,0,0,7,1); edges.addBoothWindow(0,0,3);
			require(edges.canAddAccessPanel(room,0,3,{0.5f,0.25f,0.5f}), "Fixed wall edge contact refused");
		}
		{
			core::World reverse("Furniture after panels",8,2); auto room=reverse.addRoom("Room",0,0,0,7,1);
			reverse.addAccessPanel(room,0,3,{1,1,0});
			reverse.attachFurnitureCatalogue("chair.furniture.lua",core::FurnitureCatalogue::readFile(context.fixture("src/headless/smoke/fixtures/furniture/chair.furniture.lua")));
			reverse.placeFurniture(room,"chair",3,0,"In front"); reverse.finishBuild(); reverse.pauseSimulation();
			reverse.createAgent("In front",room,0,3.5f);
			require(panelIn(reverse,room) && reverse.furniture().size()==1, "Furniture/Agent overlap invalidated panel");
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
