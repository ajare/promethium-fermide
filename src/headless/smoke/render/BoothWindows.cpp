#include "Checks.h"
#include "Render.h"
#include "WorldDrawList.h"
#include "ObjectTileset.h"
#include "UISettings.h"
#include "core/World.h"
#include <cmath>

extern UISettings gUISettings;
namespace
{
	void presentation(smoke::Context const& context)
	{
		using smoke::require;
		ImGui::GetIO().DisplaySize={1200,800}; ImGui::GetIO().Fonts->AddFontDefault(); ImGui::GetIO().Fonts->Build(); ImGui::NewFrame();
		gUISettings.visibleLayer=0; gUISettings.worldViewportWidth=1200; gUISettings.worldViewportHeight=800;
		gUISettings.worldZoom=1; gUISettings.xOffset=0; gUISettings.yOffset=0; gUISettings.renderNextLayerWireframe=false;
		auto tiles = ObjectTileset::load(context.fixture("resources/textures/objects.tileset.yaml"));
		auto openRegion=tiles.sprites.at("booth-window-open").region;
		auto closedRegion=tiles.sprites.at("booth-window-closed").region;
		auto glassRegion=tiles.sprites.at("window-clear").region;
		require(openRegion.y != closedRegion.y, "Shutter tiles are not distinct");
		setObjectTileset(std::move(tiles), reinterpret_cast<ImTextureID>(1));
		for (bool open : {false,true})
		{
			auto world=std::make_shared<core::World>("BoothWindow clipping",8,3); world->addLayer();
			world->addRoom("Front",0,0,0,7,2); world->addRoom("Middle",1,0,0,7,2);
			world->addFacade(2,0,0,7,2,0.9f,{31,73,109});
			world->addBoothWindow(0,0,3,open ? core::Window::State::Open : core::Window::State::Closed);
			world->addSectorWindow(1,0,3,1,1); world->finishBuild();
			WorldDrawList drawing({{0,0},{1200,800}}); renderWorld(world,&drawing);
			bool backdrop=false, sprite=false;
			auto region=open ? openRegion : closedRegion;
			for (auto const& command : drawing.commands()) if (auto triangle=std::get_if<WorldDrawList::Triangle>(&command))
			{
				if (triangle->colour==IM_COL32(31,73,109,255))
				{
					backdrop=true;
					auto clip=triangle->clip;
					require(clip.minimum.x >= 3.1f*64-0.01f && clip.maximum.x <= 3.9f*64+0.01f
						&& clip.minimum.y >= 720-0.01f && clip.maximum.y <= 768+0.01f, "Nested aperture leaked beyond BoothWindow geometry");
				}
				if (triangle->texture == WorldDrawList::Texture::ObjectAtlas)
				{
					auto inside=[&](SectorTileRegion r) {
						return std::all_of(std::begin(triangle->texcoords),std::end(triangle->texcoords),[&](auto uv) {
							return uv.x >= r.x/320.0f && uv.x <= (r.x+r.width)/320.0f
								&& uv.y >= r.y/480.0f && uv.y <= (r.y+r.height)/480.0f;
						});
					};
					sprite |= inside(region);
					require(inside(region) || (open && inside(glassRegion)), "Wrong BoothWindow tile or physical panel tile");
				}
			}
			require(sprite && backdrop==open, "Shutter tile selection or open/closed aperture visibility incorrect");
			// No physical Button or Interaction point accompanies either state.
			require(world->getSimulationSnapshot().interactionPoints.empty(), "BoothWindow rendered a generated panel");
			WorldDrawList outline({{0,0},{1200,800}});
			renderSector(world->getSector(0),0,LayerRenderStyle::Wireframe,false,ImColor(IM_COL32_WHITE),&outline);
			require(std::any_of(outline.commands().begin(),outline.commands().end(),[](auto const& c) {
				auto line=std::get_if<WorldDrawList::Line>(&c);
				return line && std::abs(line->from.x-3.1f*64)<0.01f;
			}),"Wireframe omitted BoothWindow frame");
			require(std::none_of(outline.commands().begin(),outline.commands().end(),[](auto const& c) {
				return std::holds_alternative<WorldDrawList::Triangle>(c);
			}),"BoothWindow wireframe leaked shutter fill, sprites, or aperture contents");
		}
		clearObjectTileset(); ImGui::Render();
	}
}
void render_smoke::registerBoothWindows(std::vector<smoke::Check>& checks)
{
	checks.push_back({"boothWindows/staticPresentationAndNestedClipping", isolated<presentation>});
}
