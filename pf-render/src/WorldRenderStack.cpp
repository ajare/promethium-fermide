#include "WorldRenderStack.h"
#include "WorldRenderSlotAllocator.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <GL/glew.h>

#include <mpp/Camera.h>
#include <mpp/ClipRectangle.h>
#include <mpp/Colour.h>
#include <mpp/Logger.h>
#include <mpp/RenderSystem.h>
#include <mpp/RenderTexture.h>
#include <mpp/ResourceManager.h>
#include <mpp/Scene.h>
#include <mpp/helper/LineBatchDataProvider.h>
#include <mpp/helper/LineBatchRenderer.h>
#include <mpp/helper/TriangleBatchDataProvider.h>
#include <mpp/helper/TriangleBatchRenderer.h>
#include <mpp/mesh/VertexTypeSpecification.h>

#include <willpower/application/resourcesystem/DirectoryResourceLocation.h>
#include <willpower/application/resourcesystem/ImageResource.h>
#include <willpower/application/resourcesystem/ImageSetResource.h>
#include <willpower/application/resourcesystem/ResourceManager.h>
#include <willpower/common/Logger.h>
#include <yaml-cpp/yaml.h>

#include "core/ManifestCatalogResolver.h"
#include "core/WorldDocument.h"
#include "AtlasInstall.h"
#include "ObjectTileset.h"
#include "SectorTileset.h"
#include "WorldDrawList.h"

namespace resources = wp::application::resourcesystem;
namespace
{
	using Float = mpp::mesh::DataTypeFloat;
	using Byte = mpp::mesh::DataTypeUnsignedByte;
	using pf::render::RedShift;
	using pf::render::GreenShift;
	using pf::render::BlueShift;
	using pf::render::AlphaShift;

	bool same(Vec2 left, Vec2 right)
	{
		return left.x == right.x && left.y == right.y;
	}

	bool same(WorldDrawList::ClipRectangle const& left,
		WorldDrawList::ClipRectangle const& right)
	{
		return same(left.minimum, right.minimum) && same(left.maximum, right.maximum);
	}

	mpp::Colour colour(PackedColourValue value)
	{
		return {
			static_cast<float>((value >> RedShift) & 0xff) / 255.0f,
			static_cast<float>((value >> GreenShift) & 0xff) / 255.0f,
			static_cast<float>((value >> BlueShift) & 0xff) / 255.0f,
			static_cast<float>((value >> AlphaShift) & 0xff) / 255.0f
		};
	}

	void bytes(PackedColourValue value, uint8_t& red, uint8_t& green, uint8_t& blue,
		uint8_t& alpha)
	{
		red = static_cast<uint8_t>((value >> RedShift) & 0xff);
		green = static_cast<uint8_t>((value >> GreenShift) & 0xff);
		blue = static_cast<uint8_t>((value >> BlueShift) & 0xff);
		alpha = static_cast<uint8_t>((value >> AlphaShift) & 0xff);
	}

	// Thick lines stay native line primitives (ADR 0010). The requested width
	// is clamped to what the driver advertises rather than being turned into
	// filled geometry, so a line remains a line at every width.
	void setNativeLineWidth(mpp::RenderSystem const& renderSystem, float width)
	{
		auto const& range = renderSystem.getCaps().aliasedLineWidthRange;
		glLineWidth(std::clamp(width, range[0], range[1]));
	}

	class TriangleProvider final
		: public mpp::helper::TriangleBatch2DDataProvider<Float, Float, Byte>
	{
	public:
		void set(std::vector<WorldDrawList::Triangle> triangles, Vec2 origin)
		{
			mTriangles = std::move(triangles);
			mOrigin = origin;
			setNumPrimitives(mTriangles.size());
		}

		void position(uint32_t, uint32_t index, float& x0, float& y0,
			float& x1, float& y1, float& x2, float& y2) override
		{
			auto const& triangle = mTriangles.at(index);
			auto transform = [this](Vec2 point)
			{
				// MPP's 2D vertex shader converts top-left-origin screen Y to
				// OpenGL coordinates. Supplying an already-inverted Y flips the
				// complete canvas a second time.
				return Vec2{ point.x - mOrigin.x, point.y - mOrigin.y };
			};
			auto const a = transform(triangle.positions[0]);
			auto const b = transform(triangle.positions[1]);
			auto const c = transform(triangle.positions[2]);
			x0 = a.x; y0 = a.y;
			x1 = b.x; y1 = b.y;
			x2 = c.x; y2 = c.y;
		}

		void texcoords(uint32_t, uint32_t index, float& u0, float& v0,
			float& u1, float& v1, float& u2, float& v2) override
		{
			auto const& uv = mTriangles.at(index).texcoords;
			u0 = uv[0].x; v0 = 1.0f - uv[0].y;
			u1 = uv[1].x; v1 = 1.0f - uv[1].y;
			u2 = uv[2].x; v2 = 1.0f - uv[2].y;
		}

		void colour(uint32_t, uint32_t index, uint8_t& red, uint8_t& green,
			uint8_t& blue, uint8_t& alpha) override
		{
			bytes(mTriangles.at(index).colour, red, green, blue, alpha);
		}

		mpp::Colour diffuse(uint32_t) override { return mpp::Colour::White; }

		void getBounds(glm::vec3& minimum, glm::vec3& maximum) override
		{
			minimum = { 0.0f, 0.0f, 0.0f };
			maximum = { 0.0f, 0.0f, 0.0f };
		}

	private:
		std::vector<WorldDrawList::Triangle> mTriangles;
		Vec2 mOrigin{};
	};

	class LineProvider final
		: public mpp::helper::LineBatchDataProvider<Float, Byte>
	{
	public:
		void set(std::vector<WorldDrawList::Line> lines, Vec2 origin)
		{
			mLines = std::move(lines);
			mOrigin = origin;
			setNumPrimitives(mLines.size());
		}

		void position(uint32_t index, float& x0, float& y0,
			float& x1, float& y1) override
		{
			auto const& line = mLines.at(index);
			x0 = line.from.x - mOrigin.x;
			y0 = line.from.y - mOrigin.y;
			x1 = line.to.x - mOrigin.x;
			y1 = line.to.y - mOrigin.y;
		}

		void colour(uint32_t index, uint8_t& red, uint8_t& green,
			uint8_t& blue, uint8_t& alpha) override
		{
			bytes(mLines.at(index).colour, red, green, blue, alpha);
		}

		mpp::Colour diffuse() override { return mpp::Colour::White; }

		void getBounds(glm::vec3& minimum, glm::vec3& maximum) override
		{
			minimum = { 0.0f, 0.0f, 0.0f };
			maximum = { 0.0f, 0.0f, 0.0f };
		}

	private:
		std::vector<WorldDrawList::Line> mLines;
		Vec2 mOrigin{};
	};

	struct Segment
	{
		enum class Kind { Triangles, Lines, Text } kind{};
		WorldDrawList::Texture texture{ WorldDrawList::Texture::None };
		float lineWidth{ 1.0f };
		WorldDrawList::ClipRectangle clip{};
		std::vector<WorldDrawList::Triangle> triangles;
		std::vector<WorldDrawList::Line> lines;
		std::vector<WorldDrawList::Text> texts;
	};

	class WorldRenderStack
	{
		struct Slot
		{
			Segment::Kind kind{};
			WorldDrawList::Texture texture{ WorldDrawList::Texture::None };
			std::shared_ptr<TriangleProvider> triangles;
			std::shared_ptr<LineProvider> lines;
			std::shared_ptr<mpp::helper::TriangleBatch2DRenderer<Float, Float, Byte>> triangleRenderer;
			std::shared_ptr<mpp::helper::LineBatchRenderer<Float, Byte>> lineRenderer;
			mpp::SceneModel2dPtr sceneModel;
		};

	public:
		WorldRenderStack(std::filesystem::path const& resourceDirectory,
			std::size_t width, std::size_t height,
			std::function<void(resources::ResourceManager&)> const& registerResourceTypes)
		{
			// Keep diagnostics in the build tree regardless of the launch directory.
			std::filesystem::path const logDirectory(PF_RENDER_LOG_DIR);
			std::filesystem::create_directories(logDirectory);
			mMppLogger = std::make_unique<mpp::Logger>();
			if (!mMppLogger->initialise(
				(logDirectory / "promethium-fermide-mpp.log").string(),
				mpp::Logger::Level::Debug))
				throw std::runtime_error("Could not initialise the MPP logger");
			mWpLogger = std::make_unique<wp::Logger>();
			mWpLogger->open((logDirectory / "promethium-fermide-resources.html").string());

			mOwnedRenderSystem = std::make_unique<mpp::RenderSystem>(width, height,
				mMppLogger.get());
			mOwnedRenderResources = std::make_unique<mpp::ResourceManager>(
				mOwnedRenderSystem.get(), mMppLogger.get());
			mOwnedRenderSystem->createCoreResources(mOwnedRenderResources.get());
			mOwnedResources = std::make_unique<resources::ResourceManager>(
				mOwnedRenderSystem.get(), mOwnedRenderResources.get(), nullptr,
				mWpLogger.get());
			mRenderSystem = mOwnedRenderSystem.get();
			mRenderResources = mOwnedRenderResources.get();
			mResources = mOwnedResources.get();
			// The application's Resource subclasses are declared above this
			// module, so the host installs them here - before the manifest is
			// scanned, because the manifest names the types it declares.
			registerResourceTypes(*mResources);
			mResources->addResourceLocationFactory("Directory",
				[logger = mWpLogger.get()](std::string const& location,
					std::string const& definition)
				{
					return new resources::DirectoryResourceLocation(
						logger, location, definition);
				});
			mResources->addResourceLocation("Directory", resourceDirectory.string(),
				"Resources.yaml");
			mResources->scanLocations();
			mCatalogResolver = std::make_shared<core::ManifestCatalogResolver>(
				resourceDirectory / "Resources.yaml");
			core::setCatalogResourceResolver(mCatalogResolver);

			mAtlases.sector = requireImageSet("SectorAtlas");
			mAtlases.objects = requireImageSet(mAtlases.objectName);
			mAtlases.agent = requireImageSet("AgentAtlas");
			mAtlases.furniture = requireImageSet("FurnitureAtlas");
			installAtlasDefinitions(/*objectsAreMandatory=*/ true);

			mScene = mRenderSystem->createScene("Default");
			mScene->load();
			mScene->setClearColour({ 0.08f, 0.08f, 0.08f, 1.0f });
		}

		// A host-bound stack (#1002). The host's render system, resource
		// managers and scene are borrowed, never owned: nothing here creates,
		// resizes or destroys them, and no second render system is made to go
		// beside the host's. The atlases are installed from the manager the host
		// already scanned, so the shared renderer draws with the same artwork
		// the host loaded rather than a second copy of it.
		//
		// Only `ObjectAtlas` is mandatory here. The game tolerates a missing or
		// foreign Sector, Agent or Furniture atlas - its Map load treats an
		// invalid artwork selection as recoverable - so those three are installed
		// through the same tolerant path a later `selectWorld*ImageSet` takes,
		// and a failure lands in the diagnostic and draws magenta.
		explicit WorldRenderStack(WorldRenderStackHost const& host)
			: mHostBound(true)
			, mRenderSystem(&host.renderSystem)
			, mRenderResources(&host.renderResources)
			, mResources(&host.resources)
			, mScene(host.scene)
		{
			mAtlases.objects = requireImageSet(mAtlases.objectName);
			installOptionalAtlas(mAtlases.sector, mAtlases.sectorName, mAtlases.sectorDiagnostic);
			installOptionalAtlas(mAtlases.agent, mAtlases.agentName, mAtlases.agentDiagnostic);
			installOptionalAtlas(mAtlases.furniture, mAtlases.furnitureName,
				mAtlases.furnitureDiagnostic);
			installAtlasDefinitions(/*objectsAreMandatory=*/ true);
		}

		~WorldRenderStack()
		{
			// Anything the host registered against the resource manager has to
			// go before the manager does, and every GPU object here has to go
			// before the OpenGL context does. This order is the guarantee.
			if (!mHostBound)
			{
				core::setCatalogResourceResolver({});
				mCatalogResolver.reset();
			}
			for (auto& slot : mSlots) remove(slot);
			mSlots.clear();
			if (!mHostBound && mScene)
			{
				mScene->unload();
				mScene.reset();
			}
			for (auto& target : mTargets) target.reset();
			mFontAtlas.reset();
			// The acquisitions this stack made are released either way: that is
			// balanced against the acquire, not against who owns the manager.
			if (mAtlases.sector) mResources->releaseResource(mAtlases.sector);
			if (mAtlases.objects) mResources->releaseResource(mAtlases.objects);
			if (mAtlases.furniture) mResources->releaseResource(mAtlases.furniture);
			if (mAtlases.agent) mResources->releaseResource(mAtlases.agent);
			clearSectorTileset();
			clearObjectTileset();
			clearAgentTileset();
			clearFurnitureTileset();
			if (mHostBound)
			{
				// The host tears its own stack down, in its own order, before its
				// OpenGL context goes. Dropping the borrowed pointers here is all
				// this side may do.
				mScene.reset();
				mResources = nullptr;
				mRenderResources = nullptr;
				mRenderSystem = nullptr;
				return;
			}
			mOwnedResources.reset();
			if (mOwnedRenderSystem) mOwnedRenderSystem->destroyCoreResources();
			mOwnedRenderResources.reset();
			mOwnedRenderSystem.reset();
			mWpLogger.reset();
			mMppLogger.reset();
		}

		OffscreenTargetId render(WorldDrawList const& commandList, Vec2 origin,
			Vec2 size, WorldCanvas canvas)
		{
			auto const width = static_cast<std::size_t>(std::max(1.0f, std::ceil(size.x)));
			auto const height = static_cast<std::size_t>(std::max(1.0f, std::ceil(size.y)));
			auto& target = mTargets[static_cast<std::size_t>(canvas)];
			ensureTarget(target, width, height, canvas);
			auto segments = buildSegments(commandList);
			prepareSlots(segments, origin);

			mRenderSystem->pushRenderTarget(target);
			mRenderSystem->setViewport(0, 0, width, height);
			mRenderSystem->clearScreen(mScene->getClearColour());
			mRenderSystem->setProjection2dOrthographic();
			mRenderSystem->resetTransform();

			for (std::size_t index = 0; index < segments.size(); ++index)
			{
				auto const& segment = segments[index];
				auto const clip = targetClip(segment.clip, origin,
					static_cast<int>(width), static_cast<int>(height));
				// MPP treats an empty scissor as no active clip. Do not submit
				// commands whose CPU-side clip intersection has no area; a closed
				// Door's zero-height aperture must not repaint over its leaf.
				if (clip.width == 0 || clip.height == 0) continue;
				mRenderSystem->pushClipRectangle(clip);
				if (segment.kind == Segment::Kind::Text)
				{
					for (auto const& text : segment.texts)
						mRenderSystem->renderText(text.value,
							static_cast<int>(text.position.x - origin.x),
							static_cast<int>(text.position.y - origin.y),
							colour(text.colour));
				}
				else
				{
					if (segment.kind == Segment::Kind::Lines)
						setNativeLineWidth(*mRenderSystem, segment.lineWidth);
					mSlots[mSegmentSlots[index]].sceneModel->render({});
					mRenderSystem->flushVertexBuffers();
				}
				mRenderSystem->popClipRectangle();
			}
			setNativeLineWidth(*mRenderSystem, 1.0f);
			mRenderSystem->popRenderTarget();

			return static_cast<std::uint64_t>(
				static_cast<mpp::RenderTexture*>(target.get())->getId());
		}

		// The host reads its catalogue through the manager this stack created;
		// it never owns a second one.
		resources::ResourceManager& resources() { return *mResources; }

		bool hostBound() const { return mHostBound; }

		// Draws a recorded stream into whatever target the host has bound
		// (#1002). The host's own presentation - the window, its viewport and
		// its clear - is left alone; this pushes the orthographic World
		// transform, submits the batches in command order under each segment's
		// aperture, and restores the matrices it pushed.
		void renderToHostTarget(WorldDrawList const& commandList, Vec2 origin,
			Vec2 size)
		{
			auto const width = static_cast<int>(std::max(1.0f, std::ceil(size.x)));
			auto const height = static_cast<int>(std::max(1.0f, std::ceil(size.y)));
			auto segments = buildSegments(commandList);
			prepareSlots(segments, origin);

			mRenderSystem->pushProjectionMatrix();
			mRenderSystem->pushModelMatrix();
			mRenderSystem->setProjection2dOrthographic();
			mRenderSystem->resetTransform();
			auto const restore = [&] {
				mRenderSystem->popModelMatrix();
				mRenderSystem->popProjectionMatrix();
				setNativeLineWidth(*mRenderSystem, 1.0f);
				for (auto& slot : mSlots)
					if (slot.sceneModel) slot.sceneModel->setVisible(false);
			};
			try
			{
				for (std::size_t index = 0; index < segments.size(); ++index)
				{
					auto const& segment = segments[index];
					auto const clip = targetClip(segment.clip, origin, width, height);
					if (clip.width == 0 || clip.height == 0) continue;
					mRenderSystem->pushClipRectangle(clip);
					try
					{
						if (segment.kind == Segment::Kind::Text)
						{
							for (auto const& text : segment.texts)
								mRenderSystem->renderText(text.value,
									static_cast<int>(text.position.x - origin.x),
									static_cast<int>(text.position.y - origin.y),
									colour(text.colour));
						}
						else
						{
							if (segment.kind == Segment::Kind::Lines)
								setNativeLineWidth(*mRenderSystem, segment.lineWidth);
							mSlots[mSegmentSlots[index]].sceneModel->render({});
							mRenderSystem->flushVertexBuffers();
						}
					}
					catch (...) { mRenderSystem->popClipRectangle(); throw; }
					mRenderSystem->popClipRectangle();
				}
			}
			catch (...) { restore(); throw; }
			restore();
		}

		// The four atlas selections, each one a call to the shared selection
		// shape below with its own slot and its own install.
		//
		// What each installs is the slot that changed and nothing else (#1035).
		// Until here a selection ended in the four-slot install pass, which
		// re-read every authored tileset behind the three atlases nobody had
		// touched; each of those reads went back to the registry on disk with
		// it. The other three slots keep whatever the last install put in them,
		// which is what the canvas draws.
		std::string selectSectorImageSet(std::string const& name)
		{
			return selectImageSet(mAtlases.sectorName, mAtlases.sector,
				mAtlases.sectorDiagnostic, name,
				[this] { installSectorSlotArtwork(); });
		}

		std::string selectAgentImageSet(std::string const& name)
		{
			return selectImageSet(mAtlases.agentName, mAtlases.agent,
				mAtlases.agentDiagnostic, name,
				[this] { installAgentSlotArtwork(); });
		}

		std::string selectFurnitureImageSet(std::string const& name)
		{
			return selectImageSet(mAtlases.furnitureName, mAtlases.furniture,
				mAtlases.furnitureDiagnostic, name,
				[this] { installFurnitureSlotArtwork(); });
		}

		// The fourth atlas selection (#1022). Unlike the other three, a bad
		// Objects atlas is not merely an absent picture: the object renderer
		// draws its Doors, Windows, Lifts and Shuttles at the extents this
		// atlas declares, so a wrong selection is refused rather than drawn.
		// The refusal lands in the diagnostic the same way an unloadable set
		// does, leaving the slot empty and magenta rather than misaligned.
		std::string selectObjectImageSet(std::string const& name)
		{
			return selectImageSet(mAtlases.objectName, mAtlases.objects,
				mAtlases.objectDiagnostic, name,
				[this] { installObjectSlotArtwork(); });
		}

		// Reloads the four selected atlases from disk, definitions and images
		// both.
		//
		// This is a refresh, not a teardown, and the shared `Image` is why it
		// has to be. The Objects, Agent and Furniture slots all reference
		// `ObjectAtlasImage` at once, and the resource graph holds its own
		// acquisitions on top of those. A reload built on release-and-destroy
		// could therefore never reach the ones that matter most: the ref-count
		// says the sheet is in use, the ref-count is right, and so the destroy
		// refuses it. A refresh says something different - the resource is
		// going back to the state it is already in, only read again - so the
		// ref-count has nothing to object to, and every holder keeps the same
		// resource object it had before.
		//
		// Both halves get refreshed. The `Image` is the resource that holds the
		// decoded PNG, so refreshing only the `ImageSet` would re-read the
		// definitions and leave the pixels exactly as they were. The image goes
		// first, so the set is re-read over a sheet that is already current.
		//
		// The render slots are deliberately left standing. A refresh keeps the
		// MPP texture the same object with new pixels behind it, so the batches
		// already holding it draw the new sheet without being torn down and
		// rebuilt - which is just as well, since that churn was never what a
		// reload needed.
		std::string reloadImageSets()
		{
			// The registry is re-read rather than reused.
			mAtlasInstaller = {};

			struct ReloadableSlot
			{
				char const* label;
				std::string* name;
				resources::ResourcePtr* resource;
				std::string* diagnostic;
				void (WorldRenderStack::*install)();
			};
			std::vector<ReloadableSlot> const slots{
				{ "Sector", &mAtlases.sectorName, &mAtlases.sector,
					&mAtlases.sectorDiagnostic,
					&WorldRenderStack::installSectorSlotArtwork },
				{ "Objects", &mAtlases.objectName, &mAtlases.objects,
					&mAtlases.objectDiagnostic,
					&WorldRenderStack::installObjectSlotArtwork },
				{ "Agent", &mAtlases.agentName, &mAtlases.agent,
					&mAtlases.agentDiagnostic,
					&WorldRenderStack::installAgentSlotArtwork },
				{ "Furniture", &mAtlases.furnitureName, &mAtlases.furniture,
					&mAtlases.furnitureDiagnostic,
					&WorldRenderStack::installFurnitureSlotArtwork } };

			std::vector<std::string> failures;
			// Three of the four slots share one `Image`, so the same resource
			// turns up in this pass several times. Once is enough.
			std::set<resources::Resource*> refreshed;

			for (auto const& slot : slots)
			{
				slot.diagnostic->clear();
				try
				{
					if (!*slot.resource)
					{
						// Nothing there to refresh; take it up the way a
						// start-up would.
						*slot.resource = requireImageSet(*slot.name);
					}
					else
					{
						// The `Image` is the resource holding the decoded
						// PNG, so it goes first: refreshing only the `ImageSet`
						// would re-read the definitions and leave the pixels
						// exactly as they were.
						auto const imageSet = std::dynamic_pointer_cast<
							resources::ImageSetResource>(*slot.resource);
						if (imageSet)
						{
							auto const pixels = imageSet->getImage();
							if (pixels && refreshed.insert(pixels.get()).second)
							{
								mResources->refreshResource(pixels);
							}
						}
						if (refreshed.insert(slot.resource->get()).second)
						{
							mResources->refreshResource(*slot.resource);
						}
					}
				}
				catch (std::exception const& error)
				{
					*slot.diagnostic = error.what();
				}
				(this->*(slot.install))();
				if (!slot.diagnostic->empty())
					failures.push_back(std::string(slot.label) + ": "
						+ *slot.diagnostic);
			}

			std::string joined;
			for (auto const& failure : failures)
			{
				if (!joined.empty()) joined += "\n";
				joined += failure;
			}
			return joined;
		}

	private:
		// The four slot installs, named so a selection and a reload ask for the
		// same thing and cannot drift apart. Each reads its slot's current name,
		// resource and diagnostic at the moment it is called, which is what
		// lets one install serve a switch to a different set and a reload of
		// the same one.
		void installSectorSlotArtwork()
		{
			mAtlasInstaller.installSectorSlot(mAtlases.sectorName, mAtlases.sector,
				mAtlases.sectorDiagnostic);
		}

		void installObjectSlotArtwork()
		{
			mAtlasInstaller.installObjectSlot(mAtlases.objectName, mAtlases.objects,
				setObjectTileset, mAtlases.objectDiagnostic,
				/*mandatory=*/ false, /*enforceObjectContract=*/ true);
		}

		void installAgentSlotArtwork()
		{
			mAtlasInstaller.installObjectSlot(mAtlases.agentName, mAtlases.agent,
				setAgentTileset, mAtlases.agentDiagnostic,
				/*mandatory=*/ false, /*enforceObjectContract=*/ false);
		}

		void installFurnitureSlotArtwork()
		{
			mAtlasInstaller.installObjectSlot(mAtlases.furnitureName,
				mAtlases.furniture, setFurnitureTileset,
				mAtlases.furnitureDiagnostic, /*mandatory=*/ false,
				/*enforceObjectContract=*/ false);
		}

		// Drops what the slot holds, takes the named set back up, and installs
		// it. This is the atlas *switch*: the old set is released and left
		// created, which is what lets Willpower reuse its decoded pixels and
		// its MPP declaration if it is ever selected again. A reload is not
		// this - see `reloadImageSets`, which has to treat all four slots as
		// one operation because of the shared `Image`.
		template <typename Install>
		std::string switchImageSet(std::string const& name,
			resources::ResourcePtr& slotResource, std::string& slotDiagnostic,
			Install const& installSlot)
		{
			for (auto& slot : mSlots) remove(slot);
			mSlots.clear();
			mSlotAllocator = {};
			slotDiagnostic.clear();
			if (slotResource) mResources->releaseResource(slotResource);
			slotResource.reset();
			try { slotResource = requireImageSet(name); }
			catch (std::exception const& error) { slotDiagnostic = error.what(); }
			installSlot();
			return slotDiagnostic;
		}

		// The shape every atlas selection shares: take the named set and
		// install that slot through the reload above. Nothing here reaches for
		// a neighbour slot, which is the whole of #1035 as this class sees it
		// - stated once, so no selection can pick the four-slot pass back up by
		// accident.
		//
		// The unchanged-name short-circuit stays the first thing done: a
		// selection that names what the slot already holds costs nothing. A
		// reload asks for the round trip explicitly, because for it the name
		// not changing is the whole point.
		template <typename Install>
		std::string selectImageSet(std::string& slotName,
			resources::ResourcePtr& slotResource, std::string& slotDiagnostic,
			std::string const& name, Install const& installSlot)
		{
			if (name == slotName) return slotDiagnostic;
			slotName = name;
			return switchImageSet(name, slotResource, slotDiagnostic, installSlot);
		}

		// A mandatory viewport atlas. Anything that is not a loadable ImageSet
		// fails the whole startup rather than leaving a canvas half-drawn.
		resources::ResourcePtr requireImageSet(std::string const& name)
		{
			auto resource = mResources->acquireResource(name);
			if (!std::dynamic_pointer_cast<resources::ImageSetResource>(resource)) {
				mResources->releaseResource(resource);
				throw std::runtime_error(name + " is not an ImageSet resource");
			}
			try
			{
				mResources->createResource(resource);
				mResources->loadResource(resource);
			}
			catch (...)
			{
				mResources->releaseResource(resource);
				throw;
			}
			return resource;
		}

		// A host-bound atlas the host can live without. A failure leaves the set
		// unset - which installs an empty tileset and paints magenta - and keeps
		// the reason for the caller instead of throwing through the host's
		// start-up.
		void installOptionalAtlas(resources::ResourcePtr& destination,
			std::string const& name, std::string& diagnostic)
		{
			try { destination = requireImageSet(name); }
			catch (std::exception const& error) { diagnostic = error.what(); }
		}

		// Installs the four viewport atlases as the tilesets the producers
		// read, which is what a stack does at start-up. A later selection
		// installs one slot, not this pass (#1035).
		//
		// The install itself is `pf::render::AtlasInstaller`, the one install
		// path every viewport atlas goes through (#1014). It lives outside this
		// class for one reason: this class cannot exist without a live OpenGL
		// context, and the install path needs nothing of the sort - a decoded
		// `ImageSet` on one side, an authored tileset file on the other, and a
		// CPU to pair them. Taking it out of the class is what lets the
		// headless Render check drive the very function the application runs
		// against the shipped manifest, rather than re-implementing the
		// pairing beside it and proving the helper while the caller went
		// untested (#1030).
		//
		// Everything the install needs is in `mAtlases`, which is handed over
		// directly: the four loaded `ImageSet`s, the name each slot is
		// installed under, and the diagnostic each install leaves behind.
		//
		// The installer is a member rather than a call because it caches: the
		// atlas-set registry is parsed once for this stack's lifetime and every
		// install after the first reads the parsed value (#1035).
		void installAtlasDefinitions(bool objectsAreMandatory)
		{
			mAtlasInstaller.installAll(mAtlases, objectsAreMandatory);
		}


		mpp::ResourcePtr texture(WorldDrawList::Texture textureKind)
		{
			if (textureKind == WorldDrawList::Texture::None) return {};
			if (textureKind == WorldDrawList::Texture::FontAtlas)
			{
				// A stance-transformed glyph is triangle geometry MPP draws
				// itself, so it needs the atlas sitting behind the UVs the
				// metrics tessellated. The application's text system hands it
				// over through the installed IFontMetrics: the renderer no
				// longer reaches into ImGui to get its pixels (#1000).
				if (!mFontAtlas)
				{
					auto const image = pf::render::requireFontMetrics().atlas();
					if (image.empty())
						throw std::runtime_error("The installed font metrics supplied no "
							"font atlas, so transformed World text has nothing to draw with.");
					mFontAtlas = mRenderSystem->createRenderTexture("world-font-atlas",
						image.width, image.height, 1, false);
					static_cast<mpp::RenderTexture*>(mFontAtlas.get())
						->uploadData(0, image.pixels);
				}
				return std::static_pointer_cast<mpp::RenderTexture>(mFontAtlas);
			}
			auto set = std::dynamic_pointer_cast<resources::ImageSetResource>(
				textureKind == WorldDrawList::Texture::SectorAtlas ? mAtlases.sector
					: textureKind == WorldDrawList::Texture::AgentAtlas ? mAtlases.agent
					: textureKind == WorldDrawList::Texture::FurnitureAtlas ? mAtlases.furniture : mAtlases.objects);
			return set->getImage()->getMppResource();
		}

		static std::vector<Segment> buildSegments(WorldDrawList const& commandList)
		{
			std::vector<Segment> result;
			for (auto const& command : commandList.commands())
			{
				if (std::holds_alternative<WorldDrawList::Barrier>(command))
				{
					if (!result.empty()) result.push_back({});
					continue;
				}
				if (auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command))
				{
					bool const append = !result.empty()
						&& result.back().kind == Segment::Kind::Triangles
						&& result.back().texture == triangle->texture
						&& same(result.back().clip, triangle->clip);
					if (!append)
					{
						Segment segment;
						segment.kind = Segment::Kind::Triangles;
						segment.texture = triangle->texture;
						segment.clip = triangle->clip;
						result.push_back(std::move(segment));
					}
					result.back().triangles.push_back(*triangle);
				}
				else if (auto const* line = std::get_if<WorldDrawList::Line>(&command))
				{
					bool const append = !result.empty()
						&& result.back().kind == Segment::Kind::Lines
						&& result.back().lineWidth == line->thickness
						&& same(result.back().clip, line->clip);
					if (!append)
					{
						Segment segment;
						segment.kind = Segment::Kind::Lines;
						segment.lineWidth = line->thickness;
						segment.clip = line->clip;
						result.push_back(std::move(segment));
					}
					result.back().lines.push_back(*line);
				}
				else if (auto const* text = std::get_if<WorldDrawList::Text>(&command))
				{
					bool const append = !result.empty()
						&& result.back().kind == Segment::Kind::Text
						&& same(result.back().clip, text->clip);
					if (!append)
					{
						Segment segment;
						segment.kind = Segment::Kind::Text;
						segment.clip = text->clip;
						result.push_back(std::move(segment));
					}
					result.back().texts.push_back(*text);
				}
			}
			std::erase_if(result, [](Segment const& segment)
				{ return segment.triangles.empty() && segment.lines.empty()
					&& segment.texts.empty(); });
			return result;
		}

		void prepareSlots(std::vector<Segment> const& segments, Vec2 origin)
		{
			mSlotAllocator.beginFrame();
			mSegmentSlots.resize(segments.size());
			for (auto& slot : mSlots)
				if (slot.sceneModel) slot.sceneModel->setVisible(false);
			for (std::size_t index = 0; index < segments.size(); ++index)
			{
				auto const& segment = segments[index];
				if (segment.kind == Segment::Kind::Text) continue;
				using Kind = WorldRenderSlotAllocator::Kind;
				auto kind = Kind::SolidTriangles;
				if (segment.kind == Segment::Kind::Lines) kind = Kind::Lines;
				else if (segment.texture == WorldDrawList::Texture::SectorAtlas)
					kind = Kind::SectorTriangles;
				else if (segment.texture == WorldDrawList::Texture::ObjectAtlas)
					kind = Kind::ObjectTriangles;
				else if (segment.texture == WorldDrawList::Texture::AgentAtlas)
					kind = Kind::AgentTriangles;
				else if (segment.texture == WorldDrawList::Texture::FurnitureAtlas)
					kind = Kind::FurnitureTriangles;
				else if (segment.texture == WorldDrawList::Texture::FontAtlas)
					kind = Kind::FontTriangles;
				auto const slotIndex = mSlotAllocator.acquire(kind);
				mSegmentSlots[index] = slotIndex;
				if (slotIndex >= mSlots.size()) mSlots.emplace_back();
				auto& slot = mSlots[slotIndex];
				if (!slot.triangleRenderer && !slot.lineRenderer)
				{
					slot.kind = segment.kind;
					slot.texture = segment.texture;
					create(slot, slotIndex);
				}
				if (slot.triangles)
				{
					slot.triangles->set(segment.triangles, origin);
					slot.triangleRenderer->update();
					slot.sceneModel->setVisible(true);
				}
				else
				{
					slot.lines->set(segment.lines, origin);
					slot.lineRenderer->update();
					slot.sceneModel->setVisible(true);
				}
			}
		}

		void create(Slot& slot, std::size_t order)
		{
			auto const name = "WorldCanvas." + std::to_string(order);
			if (slot.kind == Segment::Kind::Triangles)
			{
				slot.triangles = std::make_shared<TriangleProvider>();
				mpp::helper::TriangleBatchRendererParams options(
					false, false, false, false, false);
				slot.triangleRenderer = std::make_shared<
					mpp::helper::TriangleBatch2DRenderer<Float, Float, Byte>>(
						name, options, slot.triangles, texture(slot.texture),
						mRenderSystem, mRenderResources);
				slot.triangleRenderer->create();
				slot.sceneModel = mScene->add2dBatch(
					slot.triangles, slot.triangleRenderer, static_cast<int>(order));
				slot.sceneModel->getParams()->setModelBlend(true);
			}
			else
			{
				slot.lines = std::make_shared<LineProvider>();
				mpp::helper::LineBatchRendererParams options{ false, true, false };
				slot.lineRenderer = std::make_shared<
					mpp::helper::LineBatchRenderer<Float, Byte>>(
						name, options, slot.lines, mRenderSystem,
						mRenderResources);
				slot.lineRenderer->create();
				slot.sceneModel = mScene->add2dBatch(
					slot.lines, slot.lineRenderer, static_cast<int>(order));
				slot.sceneModel->getParams()->setModelBlend(true);
			}
		}

		void remove(Slot& slot)
		{
			if (slot.sceneModel)
			{
				mScene->remove2dBatch(slot.sceneModel);
				slot.sceneModel.reset();
			}
			slot.triangleRenderer.reset();
			slot.lineRenderer.reset();
			slot.triangles.reset();
			slot.lines.reset();
		}

		void ensureTarget(mpp::RenderTargetPtr& target, std::size_t width,
			std::size_t height, WorldCanvas canvas)
		{
			if (!target)
				target = mRenderSystem->createRenderTexture(
					canvas == WorldCanvas::World ? "WorldCanvas.Target" : "LocationPlan.Target",
					width, height, 1, false);
			else if (target->getWidth() != width || target->getHeight() != height)
				static_cast<mpp::RenderTexture*>(target.get())->resize(width, height);
			mScene->setViewport(0, 0, width, height);
		}

		static mpp::ClipRectangle targetClip(WorldDrawList::ClipRectangle clip,
			Vec2 origin, int width, int height)
		{
			int const left = std::clamp(static_cast<int>(std::floor(
				clip.minimum.x - origin.x)), 0, width);
			int const right = std::clamp(static_cast<int>(std::ceil(
				clip.maximum.x - origin.x)), 0, width);
			int const top = std::clamp(static_cast<int>(std::floor(
				clip.minimum.y - origin.y)), 0, height);
			int const bottom = std::clamp(static_cast<int>(std::ceil(
				clip.maximum.y - origin.y)), 0, height);
			return { left, height - bottom, std::max(0, right - left),
				std::max(0, bottom - top) };
		}

		// Everything the stack draws through goes via these three. In the
		// editor-shaped stack they address the owners just beneath them; in a
		// host-bound stack they address the host's objects and the owners stay
		// null, which is what keeps the destructor from tearing down a stack it
		// does not own (#1002).
		bool mHostBound{ false };
		mpp::RenderSystem* mRenderSystem{};
		mpp::ResourceManager* mRenderResources{};
		resources::ResourceManager* mResources{};
		std::unique_ptr<mpp::Logger> mMppLogger;
		std::unique_ptr<wp::Logger> mWpLogger;
		std::unique_ptr<mpp::RenderSystem> mOwnedRenderSystem;
		std::unique_ptr<mpp::ResourceManager> mOwnedRenderResources;
		std::unique_ptr<resources::ResourceManager> mOwnedResources;
		std::shared_ptr<core::ManifestCatalogResolver> mCatalogResolver;
		// The four viewport atlas slots, in the shape the install path works
		// on: which resource each slot is named for, what is loaded behind it,
		// and the diagnostic its last install produced. One struct rather than
		// twelve members because the install works on exactly this value
		// (#1030), so what the application installs and what a headless check
		// installs are the same thing installed the same way. The Objects
		// slot's default is the set every World drew from before the choice
		// existed (#1022).
		pf::render::ViewportAtlasSlots mAtlases;
		// The install path this stack installs through, and the cache that goes
		// with it: the atlas-set registry is parsed once here rather than once
		// per atlas per install (#1035).
		pf::render::AtlasInstaller mAtlasInstaller;
		mpp::ScenePtr mScene;
		// Both images are composited later by the caller, so neither canvas may
		// overwrite the other's offscreen texture during the same frame.
		mpp::RenderTargetPtr mTargets[2];
		mpp::RenderTargetPtr mFontAtlas;
		std::vector<Slot> mSlots;
		WorldRenderSlotAllocator mSlotAllocator;
		// Draw in command-stream order, independently of pooled batch identity.
		std::vector<std::size_t> mSegmentSlots;
	};

	std::unique_ptr<WorldRenderStack> gStack;

	// What every atlas-selection entry point below reports when there is no
	// render stack to select into. An empty diagnostic means the selection was
	// accepted, so a missing stack has to say so out loud: the Sector and
	// Agent entries used to answer with an empty string here, which a panel
	// cannot tell apart from an atlas that loaded (#1034).
	char const* const kNoRenderStack{"Rendering service is not initialized"};
}

void createWorldRenderStack(std::filesystem::path const& resourceDirectory,
	std::size_t drawableWidth, std::size_t drawableHeight,
	std::function<void(resources::ResourceManager&)> const& registerResourceTypes)
{
	if (gStack) throw std::logic_error("The World render stack already exists");
	gStack = std::make_unique<WorldRenderStack>(resourceDirectory, drawableWidth,
		drawableHeight, registerResourceTypes);
}

void createWorldRenderStack(WorldRenderStackHost const& host)
{
	if (gStack) throw std::logic_error("The World render stack already exists");
	gStack = std::make_unique<WorldRenderStack>(host);
}

void resizeWorldRenderStack(std::size_t, std::size_t)
{
	// Offscreen targets follow the canvas. MPP's window dimensions are used only
	// by presentation pipelines, which this render stack does not use.
}

void destroyWorldRenderStack()
{
	gStack.reset();
}

bool worldRenderStackActive()
{
	return static_cast<bool>(gStack);
}

resources::ResourceManager* worldRenderResourceManager()
{
	return gStack ? &gStack->resources() : nullptr;
}

OffscreenTargetId submitWorldCommands(WorldDrawList const& commands,
	Vec2 canvasPosition, Vec2 canvasSize, WorldCanvas canvas)
{
	if (!gStack) throw std::runtime_error("The World render stack is not initialised");
	if (gStack->hostBound())
		throw std::runtime_error("A host-bound World render stack draws into the host's "
			"target; it owns no offscreen canvas to submit to.");
	return gStack->render(commands, canvasPosition, canvasSize, canvas);
}

void renderWorldCommandsToHostTarget(WorldDrawList const& commands,
	Vec2 canvasPosition, Vec2 canvasSize)
{
	if (!gStack) throw std::runtime_error("The World render stack is not initialised");
	if (!gStack->hostBound())
		throw std::runtime_error("Only a host-bound World render stack draws into the "
			"host's target; an owning stack submits to its own offscreen canvas.");
	gStack->renderToHostTarget(commands, canvasPosition, canvasSize);
}

// The four atlas selections answer with one shape: empty means accepted,
// non-empty means refused with that reason. A missing render stack is a
// refusal, and all four report it with the same message rather than two of
// them going silent (#1034).
std::string selectWorldSectorImageSet(std::string const& name)
{
	if (!gStack) return kNoRenderStack;
	return gStack->selectSectorImageSet(name);
}

std::string selectWorldAgentImageSet(std::string const& name)
{
	if (!gStack) return kNoRenderStack;
	return gStack->selectAgentImageSet(name);
}

std::string selectWorldFurnitureImageSet(std::string const& name)
{
	if (!gStack) return kNoRenderStack;
	return gStack->selectFurnitureImageSet(name);
}

// Installs the World's selected Objects atlas (#1022). Returns the
// diagnostic: empty when the atlas was accepted, and otherwise the reason
// it was refused - which for a set that lacks a required Objects tile, or
// declares a drawn size or render offset the renderer does not draw at,
// names the tile and both values.
std::string selectWorldObjectImageSet(std::string const& name)
{
	if (!gStack) return kNoRenderStack;
	return gStack->selectObjectImageSet(name);
}

// Reloads the four currently-selected viewport atlases from disk, the
// definitions and the images both. Each slot's `ImageSet` and its paired
// `Image` are released, destroyed and taken back through create and load -
// the destroy is what clears Willpower's created flag, and without it the
// create short-circuits and the PNG is never read again - so the texture is
// rebuilt from the file as it is now. Each slot's authored tileset is read
// again and re-paired with the `ImageSet`'s regions, and the atlas-set
// registry is re-parsed rather than served from the cache this stack has
// been installing from.
//
// Returns a diagnostic naming every slot that failed to reload, one per
// line; empty means all four came back. A failed slot keeps its own
// diagnostic as well, so the Artwork panel still reports it and the slot
// draws magenta until it is fixed. With no render stack there is nothing to
// reload, and that is reported the same way a refused selection is (#1034).
std::string reloadWorldArtwork()
{
	if (!gStack) return kNoRenderStack;
	return gStack->reloadImageSets();
}
