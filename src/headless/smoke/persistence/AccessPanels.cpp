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
		world.resumeSimulation(); world.advanceTick(); world.pauseSimulation();
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
			loaded.resumeSimulation(); loaded.advanceTick();
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
