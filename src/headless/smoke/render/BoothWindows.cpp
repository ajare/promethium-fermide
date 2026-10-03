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
		auto const atlasWidth = static_cast<float>(tiles.width);
		auto const atlasHeight = static_cast<float>(tiles.height);
		auto frameRegion=tiles.sprites.at("booth-window-open").region;
		auto shutterRegion=tiles.sprites.at("booth-window-shutter").region;
		auto glassRegion=tiles.sprites.at("window-clear").region;
		require(shutterRegion.width==42 && shutterRegion.height==38, "Shutter is not a distinct inner tile");
		setObjectTileset(std::move(tiles), reinterpret_cast<ImTextureID>(1));
		auto near=[](float a,float b) { return std::abs(a-b)<0.001f; };
		// The supplied frame is 52x48, with a five-pixel inset on each edge.
		float const left=3.1f*64, right=3.9f*64, top=720, bottom=768;
		float const innerLeft=left+(right-left)*5/52, innerRight=right-(right-left)*5/52;
		float const innerTop=top+(bottom-top)*5/48, innerBottom=bottom-(bottom-top)*5/48;
		float const height=innerBottom-innerTop;
		for (bool nestedBooth : {false,true})
		{
			auto world=std::make_shared<core::World>("BoothWindow clipping",8,3); world->addLayer();
			world->addRoom("Front",0,0,0,7,2); world->addRoom("Middle",1,0,0,7,2);
			world->addFacade(2,0,0,7,2,0.9f,{31,73,109});
			auto booth=std::static_pointer_cast<const core::BoothWindow>(world->addBoothWindow(0,0,3).object);
			std::shared_ptr<const core::BoothWindow> backBooth;
			if (nestedBooth) backBooth=std::static_pointer_cast<const core::BoothWindow>(world->addBoothWindow(1,0,3,core::Window::State::Open).object);
			else world->addSectorWindow(1,0,3,1,1);
			world->finishBuild();
			auto toggle=[&](auto device) {
				core::DeviceCommand command; command.type=core::DeviceCommandType::ToggleBoothWindow;
				command.boothWindow=device->getDeviceId(); require(bool(world->submitDeviceCommand(command)),"Render command refused");
			};
			auto draw=[&](float expected) {
				require(near(booth->getProgress(),expected),"Renderer setup missed runtime progress");
				WorldDrawList drawing({{0,0},{1200,800}}); renderWorld(world,&drawing);
				bool backdrop=false, frame=false, shutter=false; int frameIndex=-1, shutterIndex=-1, backdropIndex=-1, index=0;
				float shutterMinY=10000, shutterMaxY=-10000;
				for (auto const& command : drawing.commands())
				{
					++index;
					auto triangle=std::get_if<WorldDrawList::Triangle>(&command); if (!triangle) continue;
					if (triangle->colour==IM_COL32(31,73,109,255))
					{
						backdrop=true; backdropIndex=index;
						float const openingTop=innerBottom-height*(nestedBooth ? std::min(expected,backBooth->getProgress()) : expected);
						require(near(triangle->clip.minimum.x,innerLeft) && near(triangle->clip.maximum.x,innerRight)
							&& near(triangle->clip.minimum.y,openingTop) && near(triangle->clip.maximum.y,innerBottom),
							"Back Layer escaped uncovered/nested inner aperture");
					}
					if (triangle->texture != WorldDrawList::Texture::ObjectAtlas) continue;
					auto inside=[&](SectorTileRegion r) {
						return std::all_of(std::begin(triangle->texcoords),std::end(triangle->texcoords),[&](auto uv) {
							return uv.x >= r.x/atlasWidth-0.00001f && uv.x <= (r.x+r.width)/atlasWidth+0.00001f
								&& uv.y >= r.y/atlasHeight-0.00001f && uv.y <= (r.y+r.height)/atlasHeight+0.00001f;
						});
					};
					bool const isFrame=inside(frameRegion), isShutter=inside(shutterRegion);
					require(isFrame || isShutter || (!nestedBooth && expected>0 && inside(glassRegion)),
						"Unexpected closed composite tile or physical panel visual");
					if (isFrame)
					{
						frame=true; frameIndex=index;
						for (auto p : triangle->positions) require((near(p.x,left)||near(p.x,right)) && (near(p.y,top)||near(p.y,bottom)),
							"Frame translated with shutter");
					}
					if (isShutter)
					{
						shutter=true; shutterIndex=index;
						require(triangle->clip.minimum.x>=innerLeft-0.001f && triangle->clip.maximum.x<=innerRight+0.001f
							&& triangle->clip.minimum.y>=innerTop-0.001f && triangle->clip.maximum.y<=innerBottom+0.001f,
							"Moving shutter escaped inner/nested clip");
						// Last shutter belongs to the front object; back content is drawn first.
						if (near(triangle->clip.minimum.y,innerTop))
							for (auto p : triangle->positions) { shutterMinY=std::min(shutterMinY,p.y); shutterMaxY=std::max(shutterMaxY,p.y); }
					}
				}
				require(frame && shutter && backdrop==(expected>0 && (!backBooth || backBooth->getProgress()>0)),"Frame/shutter or aperture pass missing");
				require(near(shutterMinY,innerTop-height*expected) && near(shutterMaxY,innerBottom-height*expected),
					"Shutter did not translate upwards at physical progress");
				require(shutterIndex<frameIndex && (!backdrop || backdropIndex<shutterIndex),"Aperture/shutter/frame draw order changed");
				require(world->getSimulationSnapshot().interactionPoints.size() == (nestedBooth ? 2u : 1u),
					"Invisible owned panel missing/duplicated during production rendering");
			};
			draw(0);
			if (backBooth) { toggle(backBooth); require(world->advanceTicks(24),"Nested shutter setup failed"); }
			toggle(booth); require(world->advanceTicks(12),"Opening failed"); draw(0.25f);
			require(world->advanceTicks(12),"Opening failed"); draw(0.5f);
			if (backBooth) toggle(backBooth);
			toggle(booth); require(world->advanceTicks(12),"Reversal failed"); draw(0.25f);
			toggle(booth); require(world->advanceTicks(36),"Full opening failed"); draw(1);
			WorldDrawList outline({{0,0},{1200,800}});
			renderSector(world->getSector(0),0,LayerRenderStyle::Wireframe,false,ImColor(IM_COL32_WHITE),&outline);
			require(std::any_of(outline.commands().begin(),outline.commands().end(),[&](auto const& c) {
				auto line=std::get_if<WorldDrawList::Line>(&c); return line && near(line->from.x,left);
			}),"Wireframe omitted BoothWindow frame");
			require(std::none_of(outline.commands().begin(),outline.commands().end(),[](auto const& c) {
				return std::holds_alternative<WorldDrawList::Triangle>(c);
			}),"Wireframe leaked shutter/aperture fill");
		}
		clearObjectTileset(); ImGui::Render();
	}
}
void render_smoke::registerBoothWindows(std::vector<smoke::Check>& checks)
{
	checks.push_back({"boothWindows/staticPresentationAndNestedClipping", isolated<presentation>});
}
