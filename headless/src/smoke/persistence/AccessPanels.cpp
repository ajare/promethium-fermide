#include "WorldChecks.h"
#include "core/World.h"
#include "core/YamlSerializer.h"
#include "core/BinarySerializer.h"
#include <yaml-cpp/yaml.h>

namespace persistence
{
	void accessPanels(smoke::Context const&)
	{
		using smoke::require;
		auto write=[](core::World const& world,bool binary) {
			auto serialize=[&](auto out) {
				core::SerializationWorkData work; work.markSerializedUnmodified=false;
				world.serialize(*out,work); out->serialize(); return out->getSerializedString();
			};
			return binary ? serialize(core::BinarySerializer::toString()) : serialize(core::YamlSerializer::toString());
		};
		// Movement and independent authored copies survive both persistence formats
		// and replay, including floor-Level approaches for degenerate geometry.
		for (auto geometry : {core::AccessPanelGeometry{}, {0,1,0}, {1,0,1}, {0,0,0}})
		{
			core::World moved("Moved panels",16,3);
			auto room=moved.addRoom("Room",0,0,0,5,2); auto corridor=moved.addCorridor(0,0,6,4,1);
			auto facade=moved.addFacade(0,0,11,4,2); moved.addSectorWalkway(facade,1,2);
			moved.addSectorMarker(facade,1,2.25f); moved.addSectorMarker(corridor,0,0.5f);
			auto placed=moved.addAccessPanel(room,0,2,geometry,0.15f); moved.finishBuild(); moved.pauseSimulation();
			auto object=moved.applyObjectMove(moved.planMoveSectorObject(room,placed.index,13,1));
			moved.addAccessPanel(corridor,0,8,geometry,0.15f); moved.finishBuild();
			// Surrounding replay retires the removed source panel while retaining
			// its original tail-trimming semantics and destination indices.
			auto resize=moved.planResizeLocation(room,1,0,5,2);
			require(resize.valid,"Historical removed panel blocked surrounding move"); moved.applyLocationEdit(resize);
			for (bool binary : {false,true})
			{
				auto bytes=write(moved,binary);
				std::unique_ptr<core::Serializer> reader=binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(bytes)) : std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(bytes));
				reader->deserialize(); core::SerializationWorkData work; core::World loaded("Loaded moves",1,1);
				require(loaded.deserialize(*reader,work) && write(loaded,binary)==bytes, "Moved panel persistence changed records");
				for (bool replay : {false,true})
				{
					if (replay) { loaded.resetSimulation(); loaded.pauseSimulation(); }
					unsigned count=0; core::AccessPanelId previous{};
					for (auto s : {room,corridor,facade}) for (uint32_t i=0;i<loaded.getSector(s)->getNumObjects();++i)
						if (auto p=std::dynamic_pointer_cast<const core::AccessPanelSectorObject>(loaded.getSector(s)->getObject(i)))
						{
							++count; auto panel=p->getPanel(); auto vertex=loaded.getGraph()->getVertexForObject(std::const_pointer_cast<core::AccessPanelSectorObject>(p));
							require(s!=room && panel->getId()!=previous && panel->getGeometry()==geometry && panel->getState()==core::AccessPanel::State::Closed
								&& panel->getSpeedOverride()==0.15f && panel->getLevelOffset()==(s==facade ? 1u : 0u) && vertex && !vertex->getEdges().empty()
								&& vertex->getPosition()==core::Vector2{float(p->getCellX())+0.5f,float(p->getCellY())}
								&& loaded.lookupInteractionPoint(panel->getControl(core::AccessPanel::Action::Open)), "Moved/copied panel reconstruction broken: sector="+std::to_string(s)+" vertex="+std::to_string(bool(vertex))+" edges="+std::to_string(vertex ? vertex->getEdges().size() : 0)+" geometry="+std::to_string(geometry.width)+","+std::to_string(geometry.height)+" replay="+std::to_string(replay));
							previous=panel->getId();
						}
					require(count==2 && loaded.canAddAccessPanel(room,0,2,geometry), "Moved source attachment persisted");
				}
			}
		}
		// Legacy records inherit the default; malformed speed overrides fail atomically.
		{
			core::World source("Speed persistence",8,2);
			auto room=source.addRoom("Room",0,0,0,7,1);
			source.addAccessPanel(room,0,3,{},0.125f); source.finishBuild();
			auto baseline=write(source,false);
			for (bool binary : {false,true})
			{
				auto bytes=write(source,binary);
				if (binary)
				{
					auto at=bytes.find("speed"); require(at!=std::string::npos,"Missing binary speed");
					for (unsigned i=0;i<4;++i) bytes[at+6+i]=0;
				}
				else
				{
					auto node=YAML::Load(bytes);
					for (auto record : node["construction"]) if (record["type"].as<std::string>()=="accessPanel") record["speed"]=0;
					bytes=YAML::Dump(node);
				}
				bool refused=false;
				try {
					std::unique_ptr<core::Serializer> reader=binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(bytes))
						: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(bytes));
					reader->deserialize(); core::SerializationWorkData work; source.deserialize(*reader,work);
				} catch (std::exception const&) { refused=true; }
				require(refused && write(source,false)==baseline,"Invalid persisted speed replaced live World");
			}
			auto legacy=YAML::Load(baseline); legacy["version"]=51;
			for (auto record : legacy["construction"]) if (record["type"].as<std::string>()=="accessPanel")
			{ record.remove("speed"); record.remove("hasSpeedOverride"); }
			auto reader=core::YamlSerializer::fromString(YAML::Dump(legacy)); reader->deserialize();
			core::World loaded("Legacy",1,1); core::SerializationWorkData work;
			require(loaded.deserialize(*reader,work),"Legacy panel failed loading");
			auto panel=std::static_pointer_cast<const core::AccessPanelSectorObject>(loaded.getSector(room)->getObject(0))->getPanel();
			require(!panel->getSpeedOverride() && panel->getSpeed()==core::AccessPanel::DefaultSpeed,"Legacy speed not defaulted");
		}
		// Accepted surrounding structural edits must remain canonical in both formats.
		for (bool binary : {false,true})
		{
			core::World structural("Structural round trip",12,4); structural.addLayer(); structural.addLayer();
			auto owner=structural.addRoom("Room",2,1,0,6,2); structural.addSectorWalkway(owner,1,3);
			structural.addAccessPanel(owner,1,3,{1,1,0}); auto removed=structural.addCorridor(2,0,0,6,1);
			structural.addAccessPanel(removed,0,3); structural.finishBuild(); structural.pauseSimulation();
			structural.applyLocationEdit(structural.planRemoveLocation(removed));
			owner=structural.applyLocationEdit(structural.planResizeLocation(owner,6,2,6,2));
			structural.applyDeleteLayer(structural.planDeleteLayer(0)); structural.applyDeleteLevel(structural.planDeleteLevel(0));
			auto bytes=write(structural,binary);
			std::unique_ptr<core::Serializer> reader=binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(bytes))
				: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(bytes));
			reader->deserialize(); core::SerializationWorkData work; core::World loaded("Structural loaded",1,1);
			require(loaded.deserialize(*reader,work) && write(loaded,binary)==bytes,"Structural results did not round trip");
			for (bool replay : {false,true})
			{
				if (replay) { loaded.resetSimulation(); loaded.pauseSimulation(); }
				unsigned count=0;
				for (uint32_t s=0;s<loaded.getNumSectors();++s) for (uint32_t i=0;i<loaded.getSector(s)->getNumObjects();++i)
					if (auto p=std::dynamic_pointer_cast<const core::AccessPanelSectorObject>(loaded.getSector(s)->getObject(i)))
					{
						++count; auto vertex=loaded.getGraph()->getVertexForObject(std::const_pointer_cast<core::AccessPanelSectorObject>(p));
						require(p->getCellX()==9 && p->getCellY()==2 && p->getSector()->getLayerIndex()==1
							&& p->getPanel()->getGeometry()==core::AccessPanelGeometry{1,1,0} && vertex
							&& p->getPanel()->getState()==core::AccessPanel::State::Closed
							&& loaded.lookupInteractionPoint(p->getPanel()->getControl(core::AccessPanel::Action::Open)), "Structural replay lost owned panel");
					}
				require(count==1,"Deleted owning Location panels reconstructed");
			}
		}
		// Reverse-order reconstructed wall conflicts cannot replace a live document.
		for (unsigned kind=0;kind<5;++kind) for (bool binary : {false,true})
		{
			core::World conflict("Reverse replay",8,2); auto owner=conflict.addRoom("Room",0,0,0,7,2); conflict.addRoom("Back",1,0,0,7,2);
			conflict.addAccessPanel(owner,0,3,{0,1,0});
			if (kind==0) conflict.addSectorDoor(0,0,3);
			if (kind==1) conflict.addSectorWindow(0,0,3,1,1);
			if (kind==2) conflict.addBoothWindow(0,0,3);
			if (kind==3) conflict.addSectorLightSwitch(owner,3);
			conflict.finishBuild(); conflict.pauseSimulation(); conflict.markSaved();
			auto baseline=write(conflict,false); auto graph=conflict.getGraph(); auto bytes=write(conflict,binary);
			if (binary)
			{
				auto field=kind==4 ? std::string("levelOffset") : std::string("width");
				auto offset=bytes.find(field); require(offset!=std::string::npos,"Binary panel field missing"); offset+=field.size()+1;
				bytes[offset]=kind==4 ? 1 : 0; bytes[offset+1]=0;
				bytes[offset+2]=kind==4 ? 0 : static_cast<char>(0x80); bytes[offset+3]=kind==4 ? 0 : static_cast<char>(0x3f);
			}
			else
			{
				auto node=YAML::Load(bytes);
				for (auto record : node["construction"]) if (record["type"].as<std::string>()=="accessPanel") record[kind==4 ? "levelOffset" : "width"]=1;
				bytes=YAML::Dump(node);
			}
			bool refused=false;
			try
			{
				std::unique_ptr<core::Serializer> reader=binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(bytes))
					: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(bytes));
				reader->deserialize(); core::SerializationWorkData work; conflict.deserialize(*reader,work);
			}
			catch (std::exception const&) { refused=true; }
			require(refused && write(conflict,false)==baseline && conflict.getGraph()==graph && !conflict.isModified(),"Reverse wall replay mutated live document");
		}
		core::World world("Panel persistence",10,4);
		auto room=world.addRoom("Room",0,1,1,7,2); auto corridor=world.addCorridor(0,0,1,7,1);
		auto facade=world.addFacade(1,1,1,7,2); world.addSectorWalkway(room,1,3);
		world.addAccessPanel(room,1,4,{0,1,0});
		auto edited=world.addAccessPanel(corridor,0,3); world.addAccessPanel(facade,0,3,{1,0,1});
		auto deleted=world.addAccessPanel(room,0,2); world.finishBuild(); world.pauseSimulation();
		require(world.configureAccessPanel(corridor,edited.index,{1,0.5f,0.5f}) && world.removeAccessPanel(room,deleted.index), "Replay edit fixture failed");
		auto assertPanels=[&](core::World& loaded) {
			unsigned count=0;
			for (uint32_t s=0;s<loaded.getNumSectors();++s) for (uint32_t o=0;o<loaded.getSector(s)->getNumObjects();++o)
				if (auto object=std::dynamic_pointer_cast<core::AccessPanelSectorObject>(loaded.getSector(s)->getObject(o)))
				{
					++count; auto panel=object->getPanel(); auto vertex=loaded.getGraph()->getVertexForObject(object);
					require(panel->getType()==core::AccessPanel::Type::Empty && panel->getState()==core::AccessPanel::State::Closed
						&& vertex && vertex->getPosition()==core::Vector2{float(panel->getCellX())+0.5f,float(object->getCellY())}, "Reconstruction lost cell/type/approach");
					if (s==room) require(panel->getGeometry()==core::AccessPanelGeometry{0,1,0} && panel->getLevelOffset()==1, "Walkway panel geometry/offset lost");
					if (s==corridor) require(panel->getGeometry()==core::AccessPanelGeometry{1,0.5f,0.5f}, "Configured geometry lost");
					if (s==facade) require(panel->getGeometry()==core::AccessPanelGeometry{1,0,1}, "Facade degenerate geometry lost");
				}
			require(count==3 && loaded.getSimulationSnapshot().traversalResources.empty(), "Deleted panel restored or new passage created");
		};
		auto actor = world.createAgent("Operator", corridor, 0, 2.5f);
		auto panel = std::static_pointer_cast<const core::AccessPanelSectorObject>(world.getSector(corridor)->getObject(edited.index))->getPanel();
		world.markSaved(); auto authoredYaml = write(world, false), authoredBinary = write(world, true);
		require(bool(world.requestAccessPanel(panel->getId(), core::AccessPanel::Action::Open, actor)), "Persistence Open request refused");
		world.resumeSimulation(); world.advanceTicks(180); world.pauseSimulation();
		require(panel->getState() == core::AccessPanel::State::Open && !world.isModified()
			&& write(world, false) == authoredYaml && write(world, true) == authoredBinary, "Temporary Open persisted as authored state");
		for (bool binary : {false,true})
		{
			auto bytes=write(world,binary);
			std::unique_ptr<core::Serializer> in=binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(bytes)) : std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(bytes));
			in->deserialize(); core::SerializationWorkData work; core::World loaded("Loaded",1,1);
			require(loaded.deserialize(*in,work), "Panel roundtrip refused"); assertPanels(loaded);
			require(write(loaded,binary)==bytes, "Panel roundtrip changed authored document");
			loaded.resetSimulation(); loaded.pauseSimulation(); assertPanels(loaded);
			auto restored = std::static_pointer_cast<const core::AccessPanelSectorObject>(loaded.getSector(corridor)->getObject(edited.index))->getPanel();
			require(loaded.lookupInteractionPoint(restored->getControl(core::AccessPanel::Action::Open))
				&& loaded.requestAccessPanel(restored->getId(), core::AccessPanel::Action::Open, actor), "Loaded owned interactions unusable");
			loaded.resumeSimulation(); loaded.advanceTicks(180);
			require(restored->getState() == core::AccessPanel::State::Open, "Loaded Agent operation failed");
		}
		auto baseline=write(world,false); auto node=YAML::Load(baseline); world.markSaved(); auto graph=world.getGraph();
		for (auto change : {"widthLow","widthHigh","heightLow","heightHigh","offsetLow","offsetHigh","nan","inf","sum","owner","cell","level","type","duplicate","support","legacy","missing"})
		{
			auto invalid=YAML::Clone(node); YAML::Node panel;
			// Find the first panel rather than depending on private record ordinals.
			for (auto record : invalid["construction"]) if (record["type"].as<std::string>()=="accessPanel") { panel.reset(record); break; }
			std::string c=change;
			if (c=="widthLow") panel["width"]=-0.1;
			if (c=="widthHigh") panel["width"]=1.1;
			if (c=="heightLow") panel["height"]=-0.1;
			if (c=="heightHigh") panel["height"]=1.1;
			if (c=="offsetLow") panel["yOffset"]=-0.1;
			if (c=="offsetHigh") panel["yOffset"]=1.1;
			if (c=="nan") panel["width"]=".nan";
			if (c=="inf") panel["height"]=".inf";
			if (c=="sum") panel["yOffset"]=0.01;
			if (c=="owner") panel["sectorIndex"]=99;
			if (c=="cell") panel["xOffset"]=99;
			if (c=="level") panel["levelOffset"]=99;
			if (c=="type") panel["panelType"]="equipment";
			if (c=="duplicate") invalid["construction"].push_back(YAML::Clone(panel));
			if (c=="support") panel["xOffset"]=2;
			if (c=="legacy") invalid["version"]=50;
			if (c=="missing") panel.remove("width");
			bool refused=false; try { auto in=core::YamlSerializer::fromString(YAML::Dump(invalid)); in->deserialize(); core::SerializationWorkData work; world.deserialize(*in,work); } catch (std::exception const&) { refused=true; }
			require(refused && write(world,false)==baseline && world.getGraph()==graph && !world.isModified(), "Malformed panel YAML mutated target: "+c);
		}
		for (auto field : {"width","height","yOffset"})
		{
			auto bytes=write(world,true); auto offset=bytes.find(field); require(offset!=std::string::npos,"Missing binary geometry field");
			offset+=std::string(field).size()+1; // Float tag precedes the little-endian payload.
			bytes[offset]=0; bytes[offset+1]=0; bytes[offset+2]=static_cast<char>(0x80); bytes[offset+3]=static_cast<char>(0x7f);
			bool refused=false; try { auto in=core::BinarySerializer::fromString(bytes); in->deserialize(); core::SerializationWorkData work; world.deserialize(*in,work); } catch (std::exception const&) { refused=true; }
			require(refused && write(world,false)==baseline && world.getGraph()==graph, "Malformed binary panel mutated target");
		}
		{
			core::World conflict("Loaded wall overlap",8,2); auto room=conflict.addRoom("Room",0,0,0,7,1);
			conflict.addRoom("Back",1,0,0,7,1); conflict.addBoothWindow(0,0,3);
			conflict.addAccessPanel(room,0,3,{0,0,0}); conflict.finishBuild(); conflict.pauseSimulation(); conflict.markSaved();
			auto before=write(conflict,false); auto originalGraph=conflict.getGraph(); auto invalid=YAML::Load(before);
			for (auto record : invalid["construction"]) if (record["type"].as<std::string>()=="accessPanel")
			{ record["width"]=1; record["height"]=1; }
			bool rejected=false; try { auto reader=core::YamlSerializer::fromString(YAML::Dump(invalid)); reader->deserialize(); core::SerializationWorkData work; conflict.deserialize(*reader,work); }
			catch (std::exception const&) { rejected=true; }
			require(rejected && write(conflict,false)==before && conflict.getGraph()==originalGraph && !conflict.isModified(), "Loaded panel wall overlap partially mutated World");
		}
		core::World old("No panels",6,2); old.addRoom("Room",0,0,0,5,1);
		auto legacy=YAML::Load(write(old,false)); legacy["version"]=50;
		auto in=core::YamlSerializer::fromString(YAML::Dump(legacy)); in->deserialize(); core::SerializationWorkData work;
		require(world.deserialize(*in,work) && world.getSector(0)->getNumObjects()==0, "Existing document without panels refused");
	}
}
