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
	// The inner opening a BoothWindow clips to. The authored frame is 52 x 48
	// with a five-pixel border, leaving a 42 x 38 opening; those ratios are
	// artwork facts about the frame tile, while the World cell decides how many
	// screen pixels that opening comes to.
	struct ApertureInner
	{
		float width, height;
	};

	ApertureInner boothInner(core::BoothWindow const& booth)
	{
		core::Vector2 from{}, to{};
		booth.getFullShape(from, to);
		return {
			std::abs(to.x - from.x) * CORE_CELL_WIDTH_PIXELS * 42.0f / 52.0f,
			std::abs(to.y - from.y) * CORE_LEVEL_HEIGHT_PIXELS * 38.0f / 48.0f };
	}

	// The headless canvas every check here draws into. Five Levels tall keeps
	// the same World-space coverage the checks were written against, so none
	// of them loses artwork to Level culling at the current cell size.
	float const kCanvasWidth = 1200.0f;
	float const kCanvasHeight = 5.0f * CORE_LEVEL_HEIGHT_PIXELS;

	void dumbwaiterPresentation(smoke::Context const&)
	{
		using smoke::require;
		ImGui::GetIO().DisplaySize = {kCanvasWidth, kCanvasHeight}; ImGui::GetIO().Fonts->AddFontDefault(); ImGui::GetIO().Fonts->Build(); ImGui::NewFrame();
		gUISettings.worldViewportWidth = kCanvasWidth; gUISettings.worldViewportHeight = kCanvasHeight;
		gUISettings.worldZoom = 1; gUISettings.xOffset = 0; gUISettings.yOffset = 0;
		gUISettings.renderNextLayerWireframe = false;
		for (uint32_t initial : {0u, 1u})
		{
			auto world = dumbwaiter_fixture::make();
			auto id = world->addDumbwaiter(1, 0, 2, {initial, 2}); world->finishBuild();
			// The car shows only through its landing aperture, so the most of
			// it that can ever reach the screen is that aperture's inner opening.
			auto const landingInner = boothInner(*world->lookupDumbwaiter(id)->getAperture(initial));
			for (uint32_t layer : {0u, 1u})
			{
				gUISettings.visibleLayer = layer;
				WorldDrawList drawing({{0, 0}, {kCanvasWidth, kCanvasHeight}}); renderWorld(world, &drawing);
				unsigned car = 0, here = 0, elsewhere = 0;
				for (auto const& command : drawing.commands())
					if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command))
					{
						if (triangle->colour == packColour(180, 190, 205, 255))
						{
							++car;
							if (layer == 0) require(triangle->clip.minimum.x > 2.1f * CORE_CELL_WIDTH_PIXELS
								&& triangle->clip.maximum.x < 2.9f * CORE_CELL_WIDTH_PIXELS
								&& triangle->clip.maximum.y - triangle->clip.minimum.y
									<= landingInner.height + 0.001f,
								"Car escaped owned aperture clipping");
							else require(triangle->clip.maximum.x - triangle->clip.minimum.x > CORE_CELL_WIDTH_PIXELS,
								"Selected shaft car incorrectly aperture-clipped");
						}
						if (triangle->colour == packColour(80, 200, 120, 255) || triangle->colour == packColour(200, 160, 80, 255))
						{
							for (auto p : triangle->positions) require(p.x >= (2.0f - CORE_BUTTON_SIZE * 0.5f) * CORE_CELL_WIDTH_PIXELS - 0.01f
								&& p.x <= (2.0f + CORE_BUTTON_SIZE * 0.5f) * CORE_CELL_WIDTH_PIXELS + 0.01f,
								"Landing button did not use the allocated left fallback");
							if (triangle->colour == packColour(80, 200, 120, 255)) ++here; else ++elsewhere;
						}
					}
				require(car == 2, "Initial car missing or duplicated in production draw commands");
				require(here == (layer == 0 ? 4u : 0u) && elsewhere == here, "Landing buttons not visible exclusively on landing Layer: " + std::to_string(layer) + "/" + std::to_string(here) + "/" + std::to_string(elsewhere));
			}
			gUISettings.visibleLayer = 0; gUISettings.renderNextLayerWireframe = true;
			WorldDrawList drawing({{0, 0}, {kCanvasWidth, kCanvasHeight}}); renderWorld(world, &drawing);
			unsigned car = 0;
			for (auto const& command : drawing.commands()) if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command);
				triangle && triangle->colour == packColour(180, 190, 205, 255)) ++car;
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
					WorldDrawList motion({{0, 0}, {kCanvasWidth, kCanvasHeight}}); renderWorld(world, &motion);
					unsigned busy = 0, carCount = 0, here = 0, elsewhere = 0;
					float low = 10000, high = -10000;
					for (auto const& command : motion.commands())
						if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command))
						{
							if (triangle->colour == packColour(220, 80, 80, 255)) ++busy;
							if (triangle->colour == packColour(80, 200, 120, 255)) ++here;
							if (triangle->colour == packColour(200, 160, 80, 255)) ++elsewhere;
							if (triangle->colour == packColour(180, 190, 205, 255))
							{
								++carCount;
								for (auto p : triangle->positions) { low = std::min(low, p.y); high = std::max(high, p.y); }
								if (layer == 0)
								{
									auto shutter = ticks <= 48 ? unit->getAperture(initial) : unit->getAperture(1 - initial);
									// The car may only show inside the frame's inner opening, and
									// only as far as that shutter has retracted. The 42/52 and 38/48
									// ratios are artwork facts about the frame tile; the World cell
									// decides how many screen pixels the opening comes to.
									auto const inner = boothInner(*shutter);
									require(triangle->clip.maximum.x - triangle->clip.minimum.x
											<= inner.width + 0.001f
										&& triangle->clip.maximum.y - triangle->clip.minimum.y
											<= inner.height * shutter->getProgress() + 0.001f,
										"Moving car escaped current shutter/depth clip");
								}
							}
						}
					if (layer == 1)
						require(carCount == 2 && std::abs(low - (kCanvasHeight - (unit->getCarPosition().y + 0.55f) * CORE_LEVEL_HEIGHT_PIXELS)) < 0.001f
							&& std::abs(high - (kCanvasHeight - (unit->getCarPosition().y + 0.18f) * CORE_LEVEL_HEIGHT_PIXELS)) < 0.001f,
							"Production shaft car did not follow physical intermediate position");
					else
					{
						if (ticks >= 48 && ticks <= 168) require(carCount == 0, "Closed travel apertures expose moving car");
						else require(carCount == 2, "Uncovered aperture lost car");
					}
					require(busy == (layer == 0 && ticks < 216 ? 8u : 0u)
						&& here == (layer == 0 && ticks == 216 ? 4u : 0u) && elsewhere == here,
						"Buttons did not show both busy until arrival fully opened");
				}
			}
		}
		// Production commands and hit targeting use allocator geometry, including
		// opposite landing sides and a mixed Door/Dumbwaiter vertical stack.
		for (bool mixed : {false, true})
		{
			auto world = mixed ? std::make_shared<core::World>("Mixed landing draw", 6, 3)
				: dumbwaiter_fixture::oppositeLandings();
			if (mixed)
			{
				world->addRoom("Left wall", 0, 0, 0, 1, 1);
				auto landing = world->addRoom("Landings", 0, 0, 1, 2, 2); world->addSectorWalkway(landing, 1, 1);
				world->addRoom("Door back", 1, 0, 1, 1, 1);
				auto options = core::World::RemoteControlledDoor1Options;
				options.controls[0] = true; options.controls[1] = false;
				world->addSectorDoor(0, 0, 1, options);
			}
			auto id = world->addDumbwaiter(1, 0, 2); world->finishBuild(); gUISettings.visibleLayer = 0;
			WorldDrawList drawing({{0, 0}, {kCanvasWidth, kCanvasHeight}}); renderWorld(world, &drawing);
			for (uint32_t stop = 0; stop < 2; ++stop)
			{
				auto object = dumbwaiter_fixture::control(*world, id, stop); auto button = object->_getObject();
				auto centre = button->getPosition() + button->getSize() * 0.5f;
				std::shared_ptr<const core::SectorObject> selected;
				require(world->getObjectAtPosition(0, centre.x, centre.y, &selected) == button && selected == object,
					"Landing draw/hit targeting disagrees with physical allocation");
				auto colour = stop == 0 ? packColour(80, 200, 120, 255) : packColour(200, 160, 80, 255);
				unsigned triangles = 0;
				for (auto const& command : drawing.commands())
					if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command); triangle && triangle->colour == colour)
					{
						++triangles;
						for (auto p : triangle->positions)
							require(p.x >= button->getPosition().x * CORE_CELL_WIDTH_PIXELS - 0.001f
								&& p.x <= (button->getPosition().x + button->getSize().x) * CORE_CELL_WIDTH_PIXELS + 0.001f
								&& p.y >= kCanvasHeight - (button->getPosition().y + button->getSize().y) * CORE_LEVEL_HEIGHT_PIXELS - 0.001f
								&& p.y <= kCanvasHeight - button->getPosition().y * CORE_LEVEL_HEIGHT_PIXELS + 0.001f,
								"Landing rendered at old aperture inset or incorrect stack height");
					}
				require(triangles >= 2, "Allocated landing missing from production rendering");
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
					gUISettings.visibleLayer = layer; WorldDrawList drawing({{0, 0}, {kCanvasWidth, kCanvasHeight}}); renderWorld(world, &drawing);
					unsigned car = 0, buttons = 0, busy = 0;
					for (auto const& command : drawing.commands()) if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command))
					{
						if (triangle->colour == packColour(180, 190, 205, 255))
						{
							++car; require(bool(unit), "Deleted car remains in production draw commands");
							for (auto p : triangle->positions) require(p.y >= kCanvasHeight - 1.55f * CORE_LEVEL_HEIGHT_PIXELS - 0.001f
								&& p.y <= kCanvasHeight - 1.18f * CORE_LEVEL_HEIGHT_PIXELS + 0.001f, "Reconciled car retained in-flight position");
							if (layer + 1 == unit->getLayerIndex()) require(triangle->clip.minimum.x > 2.1f * CORE_CELL_WIDTH_PIXELS
								&& triangle->clip.maximum.x < 2.9f * CORE_CELL_WIDTH_PIXELS, "Restored car escaped aperture clip");
						}
						if (triangle->colour == packColour(80, 200, 120, 255) || triangle->colour == packColour(200, 160, 80, 255)) ++buttons;
						if (triangle->colour == packColour(220, 80, 80, 255)) ++busy;
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
			auto const nestedId = world->addDumbwaiter(2, 0, 2); world->finishBuild();
			// The car reaches the screen only through its landing aperture,
			// nested inside the Window in front, so its clip can be no taller
			// than that aperture's inner opening.
			auto const nestedInner = boothInner(*world->lookupDumbwaiter(nestedId)->getAperture(0));
			gUISettings.visibleLayer = 0;
			WorldDrawList drawing({{0, 0}, {kCanvasWidth, kCanvasHeight}}); renderWorld(world, &drawing);
			unsigned car = 0;
			for (auto const& command : drawing.commands()) if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command);
				triangle && triangle->colour == packColour(180, 190, 205, 255))
			{
				++car; require(triangle->clip.minimum.x > 2.1f * CORE_CELL_WIDTH_PIXELS && triangle->clip.maximum.x < 2.9f * CORE_CELL_WIDTH_PIXELS
					&& triangle->clip.maximum.y - triangle->clip.minimum.y <= nestedInner.height + 0.001f,
					"Nested aperture clipping leaked car geometry");
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
				WorldDrawList drawing({{0,0},{kCanvasWidth,kCanvasHeight}}); renderWorld(world,&drawing);
				unsigned cars = 0, buttons = 0; float x = layer < 2 ? 4.0f : 0.0f, y = layer < 2 ? 1.0f : 3.0f;
				for (auto const& command : drawing.commands()) if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command))
				{
					if (triangle->colour == packColour(180,190,205,255))
					{
						++cars;
						for (auto p : triangle->positions)
							require(p.x > x * CORE_CELL_WIDTH_PIXELS && p.x < (x + 1) * CORE_CELL_WIDTH_PIXELS
								&& p.y >= kCanvasHeight - (y + 0.55f) * CORE_LEVEL_HEIGHT_PIXELS - 0.001f
								&& p.y <= kCanvasHeight - (y + 0.18f) * CORE_LEVEL_HEIGHT_PIXELS + 0.001f,
								"Moved/pasted car rendered at old shaft/Level");
						if (layer % 2 == 0) require(triangle->clip.minimum.x > (x + 0.1f) * CORE_CELL_WIDTH_PIXELS
							&& triangle->clip.maximum.x < (x + 0.9f) * CORE_CELL_WIDTH_PIXELS, "Moved/pasted car lost landing aperture clip");
					}
					if (triangle->colour == packColour(80,200,120,255) || triangle->colour == packColour(200,160,80,255))
					{
						++buttons;
						for (auto p : triangle->positions) require(p.x >= (x - CORE_BUTTON_SIZE * 0.5f) * CORE_CELL_WIDTH_PIXELS - 0.01f
							&& p.x <= (x + CORE_BUTTON_SIZE * 0.5f) * CORE_CELL_WIDTH_PIXELS + 0.01f, "Old-location landing button remains");
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
		ImGui::GetIO().DisplaySize = {kCanvasWidth, kCanvasHeight}; ImGui::GetIO().Fonts->AddFontDefault(); ImGui::GetIO().Fonts->Build(); ImGui::NewFrame();
		gUISettings.visibleLayer=0; gUISettings.worldViewportWidth=kCanvasWidth; gUISettings.worldViewportHeight=kCanvasHeight;
		gUISettings.worldZoom=1; gUISettings.xOffset=0; gUISettings.yOffset=0; gUISettings.renderNextLayerWireframe=false;
		auto tiles = ObjectTileset::load(context.fixture("resources/textures/objects.tileset.yaml"));
		auto const atlasWidth = static_cast<float>(tiles.width);
		auto const atlasHeight = static_cast<float>(tiles.height);
		auto frameRegion=tiles.sprites.at("booth-window-open").region;
		auto shutterRegion=tiles.sprites.at("booth-window-shutter").region;
		auto glassRegion=tiles.sprites.at("window-clear").region;
		// Artwork pixels, not World units: the authored shutter tile inside the
		// object atlas image is 42 x 38. Re-cutting the artwork moves these
		// literals; resizing a World cell must not.
		require(shutterRegion.width==42 && shutterRegion.height==38, "Shutter is not a distinct inner tile");
		setObjectTileset(std::move(tiles), static_cast<TextureId>(1));
		auto near=[](float a,float b) { return std::abs(a-b)<0.001f; };
		for (bool nestedBooth : {false,true})
		{
			auto world=std::make_shared<core::World>("BoothWindow clipping",8,3); world->addLayer();
			world->addRoom("Front",0,0,0,7,2); world->addRoom("Middle",1,0,0,7,2);
			world->addFacade(2,0,0,7,2,0.9f,{31,73,109});
			auto booth=std::static_pointer_cast<const core::BoothWindow>(world->addBoothWindow(0,0,3).object);
			// The frame artwork is 52x48 with a five-pixel inset on each edge:
			// the 5/52 and 5/48 ratios below are artwork facts about that tile.
			// left/right/top/bottom are viewport-absolute pixels for this
			// canvas, read off the BoothWindow's own World shape so they follow
			// the World cell instead of a remembered pixel count.
			core::Vector2 frameMin{}, frameMax{};
			booth->getFullShape(frameMin, frameMax);
			float const left=frameMin.x*CORE_CELL_WIDTH_PIXELS, right=frameMax.x*CORE_CELL_WIDTH_PIXELS;
			float const top=kCanvasHeight-frameMax.y*CORE_LEVEL_HEIGHT_PIXELS, bottom=kCanvasHeight-frameMin.y*CORE_LEVEL_HEIGHT_PIXELS;
			float const innerLeft=left+(right-left)*5/52, innerRight=right-(right-left)*5/52;
			float const innerTop=top+(bottom-top)*5/48, innerBottom=bottom-(bottom-top)*5/48;
			float const height=innerBottom-innerTop;
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
				WorldDrawList drawing({{0,0},{kCanvasWidth,kCanvasHeight}}); renderWorld(world,&drawing);
				bool backdrop=false, frame=false, shutter=false; int frameIndex=-1, shutterIndex=-1, backdropIndex=-1, index=0;
				float shutterMinY=10000, shutterMaxY=-10000;
				for (auto const& command : drawing.commands())
				{
					++index;
					auto triangle=std::get_if<WorldDrawList::Triangle>(&command); if (!triangle) continue;
					if (triangle->colour==packColour(31,73,109,255))
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
			WorldDrawList outline({{0,0},{kCanvasWidth,kCanvasHeight}});
			renderSector(world->getSector(0),0,LayerRenderStyle::Wireframe,false,ImColor(White),&outline);
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
