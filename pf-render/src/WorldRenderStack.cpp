#include "WorldRenderStack.h"
#include "WorldRenderSlotAllocator.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
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

			mSectorSet = requireImageSet("SectorAtlas");
			mObjectSet = requireImageSet("ObjectAtlas");
			mAgentSet = requireImageSet("AgentAtlas");
			mFurnitureSet = requireImageSet("FurnitureAtlas");
			installAtlasDefinitions();

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
			mObjectSet = requireImageSet("ObjectAtlas");
			installOptionalAtlas(mSectorSet, mSectorSetName, mSectorDiagnostic);
			installOptionalAtlas(mAgentSet, mAgentSetName, mAgentDiagnostic);
			installOptionalAtlas(mFurnitureSet, mFurnitureSetName,
				mFurnitureDiagnostic);
			installAtlasDefinitions();
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
			if (mSectorSet) mResources->releaseResource(mSectorSet);
			if (mObjectSet) mResources->releaseResource(mObjectSet);
			if (mFurnitureSet) mResources->releaseResource(mFurnitureSet);
			if (mAgentSet) mResources->releaseResource(mAgentSet);
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

		std::string selectSectorImageSet(std::string const& name)
		{
			if (name == mSectorSetName) return mSectorDiagnostic;
			mSectorSetName = name;
			for (auto& slot : mSlots) remove(slot);
			mSlots.clear();
			mSlotAllocator = {};
			mSectorDiagnostic.clear();
			if (mSectorSet) mResources->releaseResource(mSectorSet);
			mSectorSet.reset();
			try { mSectorSet = requireImageSet(name); }
			catch (std::exception const& error) { mSectorDiagnostic = error.what(); }
			installAtlasDefinitions();
			return mSectorDiagnostic;
		}

		std::string selectAgentImageSet(std::string const& name)
		{
			if (name == mAgentSetName) return mAgentDiagnostic;
			mAgentSetName = name;
			for (auto& slot : mSlots) remove(slot);
			mSlots.clear(); mSlotAllocator = {};
			mAgentDiagnostic.clear();
			if (mAgentSet) mResources->releaseResource(mAgentSet);
			mAgentSet.reset();
			try { mAgentSet = requireImageSet(name); }
			catch (std::exception const& error) { mAgentDiagnostic = error.what(); }
			installAtlasDefinitions();
			return mAgentDiagnostic;
		}

		std::string selectFurnitureImageSet(std::string const& name)
		{
			if (name == mFurnitureSetName) return mFurnitureDiagnostic;
			mFurnitureSetName = name;
			for (auto& slot : mSlots) remove(slot);
			mSlots.clear(); mSlotAllocator = {};
			mFurnitureDiagnostic.clear();
			if (mFurnitureSet) mResources->releaseResource(mFurnitureSet);
			mFurnitureSet.reset();
			try { mFurnitureSet = requireImageSet(name); }
			catch (std::exception const& error) { mFurnitureDiagnostic = error.what(); }
			installAtlasDefinitions();
			return mFurnitureDiagnostic;
		}

	private:
		// Which authored tileset format sits behind an atlas. Both produce the
		// same render semantics; only the file shape differs.
		enum class authoredFormat { ObjectTiles, SectorTiles };

		struct InstalledAtlas
		{
			int width{}, height{};
			TileArtworkMap tiles;
		};

		// Every installed atlas draws with this handle. The shared renderer
		// only separates "an atlas is installed" from "nothing is".
		static constexpr TextureId kInstalledAtlasTexture{ 1 };

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

		// Installs the viewport atlases as the tilesets the producer reads.
		// Every atlas goes through the one install path below (#1014), so the
		// two authorities - the `ImageSet` for pixels, the authored tileset
		// for render semantics - are read the same way for all four.
		//
		// A missing atlas installs an empty tileset: every lookup then misses
		// and the draw helpers paint the cell opaque magenta, so a broken
		// install is visible rather than blank. The Object atlas is mandatory,
		// so its failure is reported instead.
		void installAtlasDefinitions()
		{
			installSectorAtlas(mSectorDiagnostic);
			installObjectAtlas("ObjectAtlas", mObjectSet, setObjectTileset,
				mObjectDiagnostic, /*mandatory=*/ true);
			installObjectAtlas(mAgentSetName, mAgentSet, setAgentTileset,
				mAgentDiagnostic, /*mandatory=*/ false);
			installObjectAtlas(mFurnitureSetName, mFurnitureSet,
				setFurnitureTileset, mFurnitureDiagnostic, /*mandatory=*/ false);
		}

		// One object-shaped atlas slot. The Sector atlas is the only slot that
		// is not object-shaped; it reads through the same `atlasArtwork()` and
		// lands in its own tileset type.
		void installObjectAtlas(std::string const& setName,
			resources::ResourcePtr const& set,
			void (*install)(ObjectTileset, TextureId), std::string& diagnostic,
			bool mandatory)
		{
			ObjectTileset tiles;
			std::string error;
			if (auto const atlas = atlasArtwork(setName, set,
					authoredFormat::ObjectTiles, error))
			{
				tiles.width = atlas->width;
				tiles.height = atlas->height;
				tiles.sprites = std::move(atlas->tiles);
			}
			install(std::move(tiles), kInstalledAtlasTexture);
			if (!error.empty() && (mandatory || diagnostic.empty())) diagnostic = error;
			if (mandatory && !error.empty()) throw std::runtime_error(error);
		}

		void installSectorAtlas(std::string& diagnostic)
		{
			SectorTileset tiles;
			std::string error;
			if (auto const atlas = atlasArtwork(mSectorSetName, mSectorSet,
					authoredFormat::SectorTiles, error))
			{
				tiles.width = atlas->width;
				tiles.height = atlas->height;
				for (auto const& [name, artwork] : atlas->tiles)
				{
					// The manifest carries a sector atlas's boundary tiles
					// under a `boundary-` prefix; the tileset keeps them in
					// its own map under the bare name.
					if (name.rfind("boundary-", 0) == 0)
						tiles.boundaries.emplace(name.substr(9), artwork.region);
					else tiles.surfaces.emplace(name, artwork.region);
				}
			}
			setSectorTileset(std::move(tiles), kInstalledAtlasTexture);
			if (!error.empty() && diagnostic.empty()) diagnostic = error;
		}

		// The one install path for every viewport atlas (#1014).
		//
		// Pixel regions come from the atlas's `ImageSet` resource - the
		// runtime pixel-region contract ADR 0010 names, and the only place a
		// drawn region is taken from. The render semantics that go with each
		// tile - the tintable flag, the render offset and the drawn size -
		// come from the authored tileset that resource names with its
		// `tileset` option. Nothing here derives a tint from a tile's name or
		// a drawn size from a domain object's geometry.
		//
		// Returns nullopt and leaves `diagnostic` set when the atlas is
		// absent or cannot be paired with its authored tileset.
		std::optional<InstalledAtlas> atlasArtwork(std::string const& setName,
			resources::ResourcePtr const& set, authoredFormat format,
			std::string& diagnostic) const
		{
			diagnostic.clear();
			auto const imageSet = std::dynamic_pointer_cast<
				resources::ImageSetResource>(set);
			if (!imageSet)
			{
				diagnostic = setName
					+ " is not an installed ImageSet resource, so its atlas has no "
					"pixel regions to install";
				return std::nullopt;
			}
			try
			{
				int width{}, height{};
				auto const regions = atlasPixelRegions(*imageSet, width, height);
				auto const authored = authoredTiles(*imageSet, format);
				return InstalledAtlas{ width, height,
					installAtlasTiles(authored, regions, width, height, setName) };
			}
			catch (std::exception const& error)
			{
				diagnostic = error.what();
				return std::nullopt;
			}
		}

		// The authored render semantics behind one atlas. The object-shaped
		// atlases and the sector atlas are different authored formats, so the
		// registry declares which loader applies; both produce the same map
		// the install pairs against. The declared format is checked against
		// the slot being installed, so a mismatch is reported rather than
		// silently loading the wrong shape.
		static TileArtworkMap authoredTiles(resources::Resource const& resource,
			authoredFormat format)
		{
			auto const authored = authoredTileset(resource);
			if (authored.format != format)
				throw std::runtime_error(resource.getName() + " is installed as "
					+ authoredFormatName(format) + " but the atlas-set registry says "
					+ authoredFormatName(authored.format) + " ('"
					+ authored.path.string() + "')");
			return format == authoredFormat::SectorTiles
				? SectorTileset::load(authored.path).tiles
				: ObjectTileset::load(authored.path).sprites;
		}

		static char const* authoredFormatName(authoredFormat format)
		{
			return format == authoredFormat::SectorTiles ? "sector tiles"
				: "object tiles";
		}

		// The authored tileset behind one atlas, read from the authored
		// atlas-set registry beside the application manifest. The manifest
		// cannot name it itself: the vendored Willpower `ImageSet` schema
		// fixes the Options an `ImageSet` is allowed to carry, and extending
		// the vendored SDK was rejected as a much wider blast radius than the
		// atlas work. The tileset stays the single authority for what a tile
		// means; the registry only says which tileset is asked about, and in
		// which authored format it is written.
		struct AuthoredTileset
		{
			authoredFormat format{};
			std::filesystem::path path{};
		};

		static AuthoredTileset authoredTileset(
			resources::Resource const& resource)
		{
			auto const registry = std::filesystem::path(resource.getDefinitionFile())
				.parent_path() / "atlas-sets.yaml";
			auto uncovered = [&registry, &resource](std::string const& why)
			{
				return std::runtime_error(resource.getName() + " has no authored tileset: "
					+ why + " ('" + registry.string() + "')");
			};
			YAML::Node root;
			try { root = YAML::LoadFile(registry.string()); }
			catch (std::exception const& error)
			{
				throw std::runtime_error("Could not read the authored atlas-set registry '"
					+ registry.string() + "': " + error.what());
			}
			if (!root.IsMap() || !root["sets"].IsMap())
				throw std::runtime_error("The authored atlas-set registry '" + registry.string()
					+ "' has no `sets` map of atlas name to authored tileset");
			auto const entry = root["sets"][resource.getName()];
			if (!entry.IsMap()) throw uncovered("no map entry names it");
			auto const file = entry["tileset"].as<std::string>();
			if (file.empty()) throw uncovered("its tileset is empty");
			auto const kind = entry["kind"].as<std::string>();
			if (kind == "sector") return { authoredFormat::SectorTiles,
				registry.parent_path() / file };
			if (kind == "object") return { authoredFormat::ObjectTiles,
				registry.parent_path() / file };
			throw uncovered("its kind '" + kind + "' is neither 'sector' nor 'object'");
		}

		// The pixel regions one atlas's `ImageSet` declares, with the image
		// size they are cut from.
		static AtlasPixelRegions atlasPixelRegions(
			resources::ImageSetResource& set, int& imageWidth,
			int& imageHeight)
		{
			auto image = std::dynamic_pointer_cast<resources::ImageResource>(
				set.getImage());
			if (!image) throw std::runtime_error(set.getName()
				+ " has no Image resource to cut its regions from");
			imageWidth = image->getWidth();
			imageHeight = image->getHeight();
			AtlasPixelRegions regions;
			for (auto const& [name, definition] : set.getImageDefinitions())
				regions.emplace(name, AtlasPixelRegion{ static_cast<int>(definition.x),
					static_cast<int>(definition.y), static_cast<int>(definition.width),
					static_cast<int>(definition.height) });
			return regions;
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
				textureKind == WorldDrawList::Texture::SectorAtlas ? mSectorSet
					: textureKind == WorldDrawList::Texture::AgentAtlas ? mAgentSet
					: textureKind == WorldDrawList::Texture::FurnitureAtlas ? mFurnitureSet : mObjectSet);
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
		resources::ResourcePtr mSectorSet;
		std::string mSectorSetName{"SectorAtlas"};
		std::string mSectorDiagnostic;
		resources::ResourcePtr mAgentSet;
		std::string mAgentSetName{"AgentAtlas"};
		std::string mAgentDiagnostic;
		resources::ResourcePtr mFurnitureSet;
		std::string mFurnitureSetName{"FurnitureAtlas"};
		std::string mFurnitureDiagnostic;
		resources::ResourcePtr mObjectSet;
		std::string mObjectDiagnostic;
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

std::string selectWorldSectorImageSet(std::string const& name)
{
	if (!gStack) return {};
	return gStack->selectSectorImageSet(name);
}

std::string selectWorldAgentImageSet(std::string const& name)
{
	if (!gStack) return {};
	return gStack->selectAgentImageSet(name);
}

std::string selectWorldFurnitureImageSet(std::string const& name)
{
	if (!gStack) return "Rendering service is not initialized";
	return gStack->selectFurnitureImageSet(name);
}
