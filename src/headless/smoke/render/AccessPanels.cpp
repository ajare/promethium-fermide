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
				auto old=created.sector->getObject(created.index);
				world->pauseSimulation();
				auto object=world->applyObjectMove(world->planMoveSectorObject(owner,created.index,4,0));
				uint32_t movedIndex=0; while (world->getSector(owner)->getObject(movedIndex)!=object) ++movedIndex;
				require(!world->getGraph()->getVertexForObject(old) && world->canAddAccessPanel(owner,0,3,geometry), "Moved rendering fixture retained source attachment");
				auto previous=object;
				auto move=kind==2 ? world->planResizeFacade(owner,1,1,7,1) : world->planResizeLocation(owner,1,1,7,1);
				require(move.valid, "Renderer Location move refused: " + move.diagnostic);
				owner=world->applyLocationEdit(move);
				object=world->getSector(owner)->getObject(movedIndex);
				require(object && object->getCellX()==5 && object->getCellY()==1
					&& !world->getGraph()->getVertexForObject(std::const_pointer_cast<core::SectorObject>(previous)), "Carried panel retained source geometry/vertex");
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
				auto actor = world->createAgent("Operator", owner, 0, 4.5f);
				auto graph = world->getGraph(); auto vertex = graph->getVertexForObject(std::const_pointer_cast<core::SectorObject>(object));
				require(bool(world->requestAccessPanel(panel->getId(), core::AccessPanel::Action::Open, actor)), "Render Open request refused");
				world->resumeSimulation(); world->advanceTick();
				require(panel->getState() == core::AccessPanel::State::Open, "Render fixture did not open");
				for (auto style : {LayerRenderStyle::Solid, LayerRenderStyle::Wireframe})
				{
					WorldDrawList opened({{0,0},{1200,800}});
					renderSector(world->getSector(owner),layer,style,false,ImColor(IM_COL32_WHITE),&opened);
					unsigned crosses = 0, openFills = 0;
					for (auto const& command : opened.commands())
					{
						if (auto line = std::get_if<WorldDrawList::Line>(&command); line && line->colour == IM_COL32(100,210,180,255))
						{
							++crosses;
							for (auto p : {line->from, line->to}) require(p.x >= min.x*64-0.001f && p.x <= max.x*64+0.001f
								&& p.y >= 800-max.y*CORE_LEVEL_HEIGHT_PIXELS-0.001f && p.y <= 800-min.y*CORE_LEVEL_HEIGHT_PIXELS+0.001f, "Open expands bounds");
						}
						if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command); triangle && triangle->colour == IM_COL32(22,30,40,255)) ++openFills;
					}
					require(crosses == 2 && openFills == (!degenerate && style == LayerRenderStyle::Solid ? 2u : 0u), "Open indistinguishable on canvas/Facade/wireframe");
				}
				require(world->getGraph() == graph && graph->getVertexForObject(std::const_pointer_cast<core::SectorObject>(object)) == vertex && panel->getGeometry() == geometry, "Presentation changed geometry/topology");
				world->pauseSimulation();
				auto removal=kind==2 ? world->planRemoveFacade(owner) : world->planRemoveLocation(owner);
				require(removal.valid,"Render owner deletion refused"); world->applyLocationEdit(removal);
				require(!world->lookupAccessPanel(panel->getId()),"Renderer owner deletion retained panel");
				gSelectedSectorObject.reset();
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
