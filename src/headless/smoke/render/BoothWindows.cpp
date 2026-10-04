#include "Checks.h"
#include "Render.h"
#include "WorldDrawList.h"
#include "ObjectTileset.h"
#include "UISettings.h"
#include "core/World.h"
#include "core/Graph.h"
#include <cmath>
#include "../support/DumbwaiterFixture.h"

extern UISettings gUISettings;
namespace
{
	void dumbwaiterPresentation(smoke::Context const&)
	{
		using smoke::require;
		ImGui::GetIO().DisplaySize = {1200, 800}; ImGui::GetIO().Fonts->AddFontDefault(); ImGui::GetIO().Fonts->Build(); ImGui::NewFrame();
		gUISettings.worldViewportWidth = 1200; gUISettings.worldViewportHeight = 800;
		clearObjectTileset(); // Geometry checks below exercise the standard Button fallback.
		gUISettings.worldZoom = 1; gUISettings.xOffset = 0; gUISettings.yOffset = 0;
		gUISettings.renderNextLayerWireframe = false;
		for (uint32_t initial : {0u, 1u})
		{
			auto world = dumbwaiter_fixture::make();
			auto id = world->addDumbwaiter(1, 0, 2, {initial, 2}); world->finishBuild();
			for (uint32_t layer : {0u, 1u})
			{
				gUISettings.visibleLayer = layer;
				WorldDrawList drawing({{0, 0}, {1200, 800}}); renderWorld(world, &drawing);
				unsigned car = 0, buttons = 0;
				for (auto const& command : drawing.commands())
					if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command))
					{
						if (triangle->colour == IM_COL32(180, 190, 205, 255))
						{
							++car;
							if (layer == 0) require(triangle->clip.minimum.x > 2.1f * 64
								&& triangle->clip.maximum.x < 2.9f * 64
								&& triangle->clip.maximum.y - triangle->clip.minimum.y < 48,
								"Car escaped owned aperture clipping");
							else require(triangle->clip.maximum.x - triangle->clip.minimum.x > 64,
								"Selected shaft car incorrectly aperture-clipped");
						}
						if (triangle->colour == IM_COL32(0, 255, 128, 255))
						{
							++buttons;
							for (auto p : triangle->positions) require(p.x >= (3.0f - CORE_BUTTON_SIZE * 0.5f) * 64 - 0.01f
								&& p.x <= (3.0f + CORE_BUTTON_SIZE * 0.5f) * 64 + 0.01f,
								"Landing Button is not the standard size at the cell border");
						}
					}
				require(car == 2, "Initial car missing or duplicated in production draw commands");
				require(buttons == (layer == 0 ? 8u : 0u), "Landing Buttons not visible exclusively on landing Layer: " + std::to_string(layer) + " count=" + std::to_string(buttons));
			}
			gUISettings.visibleLayer = 0; gUISettings.renderNextLayerWireframe = true;
			WorldDrawList drawing({{0, 0}, {1200, 800}}); renderWorld(world, &drawing);
			unsigned car = 0;
			for (auto const& command : drawing.commands()) if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command);
				triangle && triangle->colour == IM_COL32(180, 190, 205, 255)) ++car;
			require(car == 2, "Wireframe leaked a second filled car over landing Layer");
			gUISettings.renderNextLayerWireframe = false;
			auto unit = world->lookupDumbwaiter(id);
			require(bool(unit), "Runtime render fixture lost unit");
			world->resumeSimulation(); world->pressDumbwaiterLanding(unit->getId(), initial);
			unsigned elapsed = 0;
			for (unsigned ticks : {12u, 48u, 108u, 168u, 192u, 216u})
			{
				require(world->advanceTicks(ticks - elapsed), "Runtime render tick failed"); elapsed = ticks;
				for (uint32_t layer : {0u, 1u})
				{
					gUISettings.visibleLayer = layer;
					WorldDrawList motion({{0, 0}, {1200, 800}}); renderWorld(world, &motion);
					unsigned buttons = 0, carCount = 0;
					float low = 10000, high = -10000;
					for (auto const& command : motion.commands())
						if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command))
						{
							if (triangle->colour == IM_COL32(0, 255, 128, 255)) ++buttons;
							if (triangle->colour == IM_COL32(180, 190, 205, 255))
							{
								++carCount;
								for (auto p : triangle->positions) { low = std::min(low, p.y); high = std::max(high, p.y); }
								if (layer == 0)
								{
									auto shutter = ticks <= 48 ? unit->getAperture(initial) : unit->getAperture(1 - initial);
									auto fullHeight = 48.0f * 38 / 48;
									require(triangle->clip.maximum.x - triangle->clip.minimum.x < 52
										&& triangle->clip.maximum.y - triangle->clip.minimum.y <= fullHeight * shutter->getProgress() + 0.001f,
										"Moving car escaped current shutter/depth clip");
								}
							}
						}
					if (layer == 1)
						require(carCount == 2 && std::abs(low - (800 - (unit->getCarPosition().y + 0.55f) * CORE_LEVEL_HEIGHT_PIXELS)) < 0.001f
							&& std::abs(high - (800 - (unit->getCarPosition().y + 0.18f) * CORE_LEVEL_HEIGHT_PIXELS)) < 0.001f,
							"Production shaft car did not follow physical intermediate position");
					else
					{
						if (ticks >= 48 && ticks <= 168) require(carCount == 0, "Closed travel apertures expose moving car");
						else require(carCount == 2, "Uncovered aperture lost car");
					}
					require(buttons == (layer == 0 ? 8u : 0u), "Standard landing Buttons disappeared during operation");
				}
			}
		}
		for (unsigned edit = 0; edit < 4; ++edit)
		{
			auto world = dumbwaiter_fixture::make(0, true, 2); world->addLayer();
			auto id = world->addDumbwaiter(2, 0, 2, {1, 2}); world->finishBuild();
			auto authored = dumbwaiter_fixture::yaml(*world);
			world->pressDumbwaiterLanding(id, 0); world->resumeSimulation(); require(world->advanceTicks(80), "Render reconciliation fixture failed"); world->pauseSimulation();
			if (edit == 0) world->applyWalkwayEdit(world->planRemoveSectorWalkway(0, 0));
			if (edit == 1) world->applyDeleteLayer(world->planDeleteLayer(0));
			if (edit == 2) world->applyLocationEdit(world->planResizeLocation(0, 2, 0, 2, 2));
			if (edit == 3) world->applyDeleteLevel(world->planDeleteLevel(0));
			for (bool restored : {false, true})
			{
				if (restored)
				{
					auto reader = core::YamlSerializer::fromString(authored); reader->deserialize(); core::SerializationWorkData work;
					require(world->deserialize(*reader, work), "Render restoration refused"); world->pauseSimulation();
				}
				auto unit = world->lookupDumbwaiter(id);
				for (uint32_t layer = 0; layer < world->getLayerCount(); ++layer)
				{
					gUISettings.visibleLayer = layer; WorldDrawList drawing({{0, 0}, {1200, 800}}); renderWorld(world, &drawing);
					unsigned car = 0, buttons = 0, busy = 0;
					for (auto const& command : drawing.commands()) if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command))
					{
						if (triangle->colour == IM_COL32(180, 190, 205, 255))
						{
							++car; require(bool(unit), "Deleted car remains in production draw commands");
							for (auto p : triangle->positions) require(p.y >= 800 - 1.55f * CORE_LEVEL_HEIGHT_PIXELS - 0.001f
								&& p.y <= 800 - 1.18f * CORE_LEVEL_HEIGHT_PIXELS + 0.001f, "Reconciled car retained in-flight position");
							if (layer + 1 == unit->getLayerIndex()) require(triangle->clip.minimum.x > 2.1f * 64
								&& triangle->clip.maximum.x < 2.9f * 64, "Restored car escaped aperture clip");
						}
						if (triangle->colour == IM_COL32(0, 255, 128, 255)) ++buttons;
						if (triangle->colour == IM_COL32(220, 80, 80, 255)) ++busy;
					}
					bool visible = unit && (layer == unit->getLayerIndex() || layer + 1 == unit->getLayerIndex());
					require(car == (visible ? 2u : 0u) && buttons == (unit && layer + 1 == unit->getLayerIndex() ? 8u : 0u)
						&& busy == 0, "Reconciled/restored presentation duplicated components or retained busy buttons");
				}
				if (unit) for (auto const& vertex : world->getGraph()->getVertices())
					require(vertex->getSector() != unit, "Restored shaft became traversable");
				require(world->getSimulationSnapshot().traversalResources.empty(), "Reconciliation introduced passenger resources");
			}
		}
		{
			auto world = dumbwaiter_fixture::make(0, true, 2);
			world->addRoom("Front", 0, 0, 2, 1, 2);
			world->addSectorWindow(0, 0, 2, 1, 2);
			world->addDumbwaiter(2, 0, 2); world->finishBuild();
			gUISettings.visibleLayer = 0;
			WorldDrawList drawing({{0, 0}, {1200, 800}}); renderWorld(world, &drawing);
			unsigned car = 0;
			for (auto const& command : drawing.commands()) if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command);
				triangle && triangle->colour == IM_COL32(180, 190, 205, 255))
			{
				++car; require(triangle->clip.minimum.x > 2.1f * 64 && triangle->clip.maximum.x < 2.9f * 64
					&& triangle->clip.maximum.y - triangle->clip.minimum.y < 48, "Nested aperture clipping leaked car geometry");
			}
			require(car == 2, "Recursive aperture lost initial car");
		}
		{
			auto world = dumbwaiter_fixture::make();
			dumbwaiter_fixture::addLandings(*world, 3, 0, 2);
			dumbwaiter_fixture::addLandings(*world, 1, 4);
			auto id = world->addDumbwaiter(1,0,2,{1,0.5f}); world->finishBuild();
			require(world->applyDumbwaiterMove(world->planMoveDumbwaiter(id,3,2,0)), "Render movement fixture failed");
			world->addDumbwaiter(1,0,4,{1,0.5f}); world->finishBuild();
			for (uint32_t layer = 0; layer < 4; ++layer)
			{
				gUISettings.visibleLayer = layer;
				WorldDrawList drawing({{0,0},{1200,800}}); renderWorld(world,&drawing);
				unsigned cars = 0, buttons = 0; float x = layer < 2 ? 4.0f : 0.0f, y = layer < 2 ? 1.0f : 3.0f;
				for (auto const& command : drawing.commands()) if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command))
				{
					if (triangle->colour == IM_COL32(180,190,205,255))
					{
						++cars;
						for (auto p : triangle->positions)
							require(p.x > x * 64 && p.x < (x + 1) * 64
								&& p.y >= 800 - (y + 0.55f) * CORE_LEVEL_HEIGHT_PIXELS - 0.001f
								&& p.y <= 800 - (y + 0.18f) * CORE_LEVEL_HEIGHT_PIXELS + 0.001f,
								"Moved/pasted car rendered at old shaft/Level");
						if (layer % 2 == 0) require(triangle->clip.minimum.x > (x + 0.1f) * 64
							&& triangle->clip.maximum.x < (x + 0.9f) * 64, "Moved/pasted car lost landing aperture clip");
					}
					if (triangle->colour == IM_COL32(0,255,128,255))
					{
						++buttons;
						for (auto p : triangle->positions) require(p.x >= (x + 1 - CORE_BUTTON_SIZE * 0.5f) * 64 - 0.01f
							&& p.x <= (x + 1 + CORE_BUTTON_SIZE * 0.5f) * 64 + 0.01f, "Old-location landing Button remains");
					}
				}
				require(cars == 2 && buttons == (layer % 2 == 0 ? 8u : 0u), "Moved/pasted rendering duplicated/lost components");
			}
		}
		ImGui::Render();
	}
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
	checks.push_back({"dumbwaiters/initialPresentationAndClipping", isolated<dumbwaiterPresentation>});
	checks.push_back({"boothWindows/staticPresentationAndNestedClipping", isolated<presentation>});
}
