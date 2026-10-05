#include "Checks.h"
#include "Render.h"
#include "WorldDrawList.h"
#include "UISettings.h"
#include "core/World.h"
#include <cmath>

extern UISettings gUISettings;
extern std::shared_ptr<const core::SectorObject> gSelectedSectorObject;
namespace
{
	void presentation(smoke::Context const&)
	{
		using smoke::require;
		ImGui::GetIO().DisplaySize={1200,800}; ImGui::GetIO().Fonts->AddFontDefault(); ImGui::GetIO().Fonts->Build(); ImGui::NewFrame();
		gUISettings.worldViewportWidth=1200; gUISettings.worldViewportHeight=800; gUISettings.worldZoom=1;
		gUISettings.xOffset=0; gUISettings.yOffset=0; gUISettings.renderNextLayerWireframe=false;
		for (unsigned kind=0;kind<3;++kind) for (uint32_t layer : {0u,1u})
			for (auto geometry : {core::AccessPanelGeometry{}, {0,0.25f,0.25f}, {0.5f,0,0.25f}, {0,0,1}})
			{
				auto world=std::make_shared<core::World>("Panel rendering",8,3);
				auto owner=kind==0 ? world->addRoom("Room",layer,0,0,7,1) : kind==1 ? world->addCorridor(layer,0,0,7,1) : world->addFacade(layer,0,0,7,1);
				auto created=world->addAccessPanel(owner,0,3,geometry); world->finishBuild();
				auto object=created.sector->getObject(created.index);
				auto panel=std::static_pointer_cast<const core::AccessPanelSectorObject>(object)->getPanel();
				core::Vector2 min,max; panel->getSelectionShape(min,max);
				bool degenerate=geometry.width==0 || geometry.height==0;
				gUISettings.visibleLayer=layer;
				for (bool selected : {false,true})
				{
					gSelectedSectorObject=selected ? object : nullptr;
					WorldDrawList drawing({{0,0},{1200,800}}); renderWorld(world,&drawing);
					unsigned fills=0,lines=0;
					for (auto const& command : drawing.commands())
					{
						if (auto triangle=std::get_if<WorldDrawList::Triangle>(&command); triangle && triangle->colour==IM_COL32(78,92,110,255))
						{
							++fills;
							for (auto p : triangle->positions) require(p.x>=min.x*64-0.001f && p.x<=max.x*64+0.001f
								&& p.y>=800-max.y*CORE_LEVEL_HEIGHT_PIXELS-0.001f && p.y<=800-min.y*CORE_LEVEL_HEIGHT_PIXELS+0.001f,"Panel fill changed authored bounds");
						}
						if (auto line=std::get_if<WorldDrawList::Line>(&command); line && line->colour==(selected ? IM_COL32(251,188,4,255) : IM_COL32(165,180,198,255)))
						{
							++lines;
							for (auto p : {line->from,line->to}) require(p.x>=min.x*64-0.001f && p.x<=max.x*64+0.001f
								&& p.y>=800-max.y*CORE_LEVEL_HEIGHT_PIXELS-0.001f && p.y<=800-min.y*CORE_LEVEL_HEIGHT_PIXELS+0.001f,"Panel outline/indicator disagrees with hit bounds");
						}
					}
					require(fills==(degenerate ? 0u : 2u) && lines==4,"Closed panel/selection indicator missing or duplicated");
					require(panel->getGeometry()==geometry,"Editor indicator changed authored geometry");
					WorldDrawList wire({{0,0},{1200,800}});
					renderSector(world->getSector(owner),layer,LayerRenderStyle::Wireframe,false,ImColor(IM_COL32_WHITE),&wire);
					require(std::none_of(wire.commands().begin(),wire.commands().end(),[](auto const& c) {return std::holds_alternative<WorldDrawList::Triangle>(c);}),"Wireframe fills panel");
				}
				world->pauseSimulation(); require(world->removeAccessPanel(owner,created.index),"Render fixture deletion refused");
				WorldDrawList after({{0,0},{1200,800}}); renderWorld(world,&after);
				require(std::none_of(after.commands().begin(),after.commands().end(),[](auto const& c) {
					auto t=std::get_if<WorldDrawList::Triangle>(&c); auto l=std::get_if<WorldDrawList::Line>(&c);
					return (t && t->colour==IM_COL32(78,92,110,255)) || (l && (l->colour==IM_COL32(165,180,198,255) || l->colour==IM_COL32(251,188,4,255)));
				}),"Deleted panel remains rendered/selectable");
			}
		gSelectedSectorObject.reset(); ImGui::Render();
	}
}
void render_smoke::registerAccessPanels(std::vector<smoke::Check>& checks)
{
	checks.push_back({"accessPanels/closedPresentationAndIndicators",isolated<presentation>});
}
