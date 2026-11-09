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
	// The fixture every check here draws: 8 cells by 3 Levels.
	uint32_t const kFixtureCellsWide = 8;
	uint32_t const kFixtureLevelsHigh = 3;

	// The headless canvas every check here draws into, sized from the World
	// cell the way BoothWindows.cpp sizes its own. The canvas has to cover the
	// whole fixture World: at a 320-pixel Level a remembered 800-pixel canvas
	// reaches only 2.5 Levels, so the top Level falls outside the clip and any
	// check tallied against it goes quietly empty.
	float const kCanvasWidth = float(kFixtureCellsWide) * CORE_CELL_WIDTH_PIXELS;
	float const kCanvasHeight = float(kFixtureLevelsHigh) * CORE_LEVEL_HEIGHT_PIXELS;

	void presentation(smoke::Context const&)
	{
		using smoke::require;
		ImGui::GetIO().DisplaySize={kCanvasWidth,kCanvasHeight}; ImGui::GetIO().Fonts->AddFontDefault(); ImGui::GetIO().Fonts->Build(); ImGui::NewFrame();
		gUISettings.worldViewportWidth=kCanvasWidth; gUISettings.worldViewportHeight=kCanvasHeight; gUISettings.worldZoom=1;
		gUISettings.xOffset=0; gUISettings.yOffset=0; gUISettings.renderNextLayerWireframe=false;
		for (unsigned kind=0;kind<3;++kind) for (uint32_t layer : {0u,1u})
			for (auto geometry : {core::AccessPanelGeometry{}, {0,0.25f,0.25f}, {0.5f,0,0.25f}, {0,0,1}})
			{
				auto world=std::make_shared<core::World>("Panel rendering",kFixtureCellsWide,kFixtureLevelsHigh);
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
					WorldDrawList drawing({{0,0},{kCanvasWidth,kCanvasHeight}}); renderWorld(world,&drawing);
					unsigned fills=0,lines=0;
					for (auto const& command : drawing.commands())
					{
						if (auto triangle=std::get_if<WorldDrawList::Triangle>(&command); triangle && triangle->colour==packColour(78,92,110,255))
						{
							++fills;
							for (auto p : triangle->positions) require(p.x>=min.x*CORE_CELL_WIDTH_PIXELS-0.001f && p.x<=max.x*CORE_CELL_WIDTH_PIXELS+0.001f
								&& p.y>=kCanvasHeight-max.y*CORE_LEVEL_HEIGHT_PIXELS-0.001f && p.y<=kCanvasHeight-min.y*CORE_LEVEL_HEIGHT_PIXELS+0.001f,"Panel fill changed authored bounds");
						}
						if (auto line=std::get_if<WorldDrawList::Line>(&command); line && line->colour==(selected ? packColour(251,188,4,255) : packColour(165,180,198,255)))
						{
							++lines;
							for (auto p : {line->from,line->to}) require(p.x>=min.x*CORE_CELL_WIDTH_PIXELS-0.001f && p.x<=max.x*CORE_CELL_WIDTH_PIXELS+0.001f
								&& p.y>=kCanvasHeight-max.y*CORE_LEVEL_HEIGHT_PIXELS-0.001f && p.y<=kCanvasHeight-min.y*CORE_LEVEL_HEIGHT_PIXELS+0.001f,"Panel outline/indicator disagrees with hit bounds");
						}
					}
					require(fills==(degenerate ? 0u : 2u) && lines==4,"Closed panel/selection indicator missing or duplicated");
					require(panel->getGeometry()==geometry,"Editor indicator changed authored geometry");
					WorldDrawList wire({{0,0},{kCanvasWidth,kCanvasHeight}});
					renderSector(world->getSector(owner),layer,LayerRenderStyle::Wireframe,false,ImColor(White),&wire);
					require(std::none_of(wire.commands().begin(),wire.commands().end(),[](auto const& c) {return std::holds_alternative<WorldDrawList::Triangle>(c);}),"Wireframe fills panel");
				}
				auto actor = world->createAgent("Operator", owner, 0, 4.5f);
				auto graph = world->getGraph(); auto vertex = graph->getVertexForObject(std::const_pointer_cast<core::SectorObject>(object));
				require(bool(world->requestAccessPanel(panel->getId(), core::AccessPanel::Action::Open, actor)), "Render Open request refused");
				world->resumeSimulation();
				for (unsigned ticks=0; panel->getState()==core::AccessPanel::State::Closed && ticks<600; ++ticks) world->advanceTick();
				world->advanceTicks(5);
				if (!degenerate)
				{
					require(panel->getState()==core::AccessPanel::State::Opening, "Missing partial opening");
					WorldDrawList partial({{0,0},{kCanvasWidth,kCanvasHeight}});
					renderSector(world->getSector(owner),layer,LayerRenderStyle::Solid,false,ImColor(White),&partial);
					unsigned leaves=0;
					float bottom=kCanvasHeight-(min.y+(max.y-min.y)*panel->getProgress())*CORE_LEVEL_HEIGHT_PIXELS;
					for (auto const& command : partial.commands())
						if (auto t=std::get_if<WorldDrawList::Triangle>(&command); t && t->colour==packColour(78,92,110,255))
						{
							++leaves;
							for (auto p : t->positions) require(p.y<=bottom+0.001f && p.y>=kCanvasHeight-max.y*CORE_LEVEL_HEIGHT_PIXELS-0.001f,
								"Panel leaf did not retract upward into its top edge");
						}
					require(leaves==2,"Partial leaf not drawn");
				}
				world->advanceTicks(180);
				require(panel->getState() == core::AccessPanel::State::Open, "Render fixture did not open");
				for (auto style : {LayerRenderStyle::Solid, LayerRenderStyle::Wireframe})
				{
					WorldDrawList opened({{0,0},{kCanvasWidth,kCanvasHeight}});
					renderSector(world->getSector(owner),layer,style,false,ImColor(White),&opened);
					unsigned crosses = 0, openFills = 0;
					for (auto const& command : opened.commands())
					{
						if (auto line = std::get_if<WorldDrawList::Line>(&command); line && line->colour == packColour(100,210,180,255))
						{
							++crosses;
							for (auto p : {line->from, line->to}) require(p.x >= min.x*CORE_CELL_WIDTH_PIXELS-0.001f && p.x <= max.x*CORE_CELL_WIDTH_PIXELS+0.001f
								&& p.y >= kCanvasHeight-max.y*CORE_LEVEL_HEIGHT_PIXELS-0.001f && p.y <= kCanvasHeight-min.y*CORE_LEVEL_HEIGHT_PIXELS+0.001f, "Open expands bounds");
						}
						if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command); triangle && triangle->colour == packColour(22,30,40,255)) ++openFills;
					}
					require(crosses == 2 && openFills == (!degenerate && style == LayerRenderStyle::Solid ? 2u : 0u), "Open indistinguishable on canvas/Facade/wireframe");
				}
				require(world->getGraph() == graph && graph->getVertexForObject(std::const_pointer_cast<core::SectorObject>(object)) == vertex && panel->getGeometry() == geometry, "Presentation changed geometry/topology");
				world->pauseSimulation();
				auto removal=kind==2 ? world->planRemoveFacade(owner) : world->planRemoveLocation(owner);
				require(removal.valid,"Render owner deletion refused"); world->applyLocationEdit(removal);
				require(!world->lookupAccessPanel(panel->getId()),"Renderer owner deletion retained panel");
				gSelectedSectorObject.reset();
				WorldDrawList after({{0,0},{kCanvasWidth,kCanvasHeight}}); renderWorld(world,&after);
				require(std::none_of(after.commands().begin(),after.commands().end(),[](auto const& c) {
					auto t=std::get_if<WorldDrawList::Triangle>(&c); auto l=std::get_if<WorldDrawList::Line>(&c);
					return (t && t->colour==packColour(78,92,110,255)) || (l && (l->colour==packColour(165,180,198,255) || l->colour==packColour(251,188,4,255)));
				}),"Deleted panel remains rendered/selectable");
			}
		gSelectedSectorObject.reset(); ImGui::Render();
	}

	// The coverage pin for the canvas above. A full-cell panel placed in the
	// fixture's topmost Level reaches the World's top edge exactly, so it is
	// the geometry a short canvas loses: with an 800-pixel canvas its top edge
	// lands 160 pixels above the clip, and with a canvas short enough to cull
	// the Sector it is not drawn at all. Either way this check goes red, so a
	// future shrink of the canvas cannot leave the checks above tallying Levels
	// they no longer reach.
	void canvasCoversTheWholeFixtureWorld(smoke::Context const&)
	{
		using smoke::require;
		ImGui::GetIO().DisplaySize={kCanvasWidth,kCanvasHeight}; ImGui::GetIO().Fonts->AddFontDefault(); ImGui::GetIO().Fonts->Build(); ImGui::NewFrame();
		gUISettings.worldViewportWidth=kCanvasWidth; gUISettings.worldViewportHeight=kCanvasHeight; gUISettings.worldZoom=1;
		gUISettings.xOffset=0; gUISettings.yOffset=0; gUISettings.renderNextLayerWireframe=false;
		gUISettings.visibleLayer=0;
		auto world=std::make_shared<core::World>("Panel canvas coverage",kFixtureCellsWide,kFixtureLevelsHigh);
		auto top=world->addRoom("Top level",0,kFixtureLevelsHigh-1,0,kFixtureCellsWide-1,1);
		auto created=world->addAccessPanel(top,0,3,{1.0f,1.0f,0.0f});
		world->finishBuild();
		auto panel=std::static_pointer_cast<const core::AccessPanelSectorObject>(
			world->getSector(top)->getObject(created.index))->getPanel();
		core::Vector2 min,max; panel->getSelectionShape(min,max);
		require(min.y==float(kFixtureLevelsHigh)-1.0f && max.y==float(kFixtureLevelsHigh)
			&& max.x-min.x==1.0f, "Coverage probe is not a full-cell panel on the fixture's topmost Level");
		WorldDrawList drawing({{0,0},{kCanvasWidth,kCanvasHeight}}); renderWorld(world,&drawing);
		unsigned fills=0;
		for (auto const& command : drawing.commands())
			if (auto triangle=std::get_if<WorldDrawList::Triangle>(&command); triangle && triangle->colour==packColour(78,92,110,255))
			{
				++fills;
				for (auto p : triangle->positions) require(p.x>=-0.001f && p.x<=kCanvasWidth+0.001f
					&& p.y>=-0.001f && p.y<=kCanvasHeight+0.001f,
					"Top-Level panel geometry fell outside the canvas");
			}
		require(fills==2,"Top-Level panel was culled rather than drawn: the canvas no longer reaches the fixture's top Level");
		ImGui::Render();
	}
}
void render_smoke::registerAccessPanels(std::vector<smoke::Check>& checks)
{
	checks.push_back({"accessPanels/closedPresentationAndIndicators",isolated<presentation>});
	checks.push_back({"accessPanels/canvasCoversTheWholeFixtureWorld",isolated<canvasCoversTheWholeFixtureWorld>});
}
