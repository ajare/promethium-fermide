#include "WorldRenderer.h"
#include <core/World.h>
#include <core/Agent.h>
#include <set>
#include "WorldRenderSlotAllocator.h"
#include "SectorTileset.h"
#include "ObjectTileset.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <GL/glew.h>
#include <mpp/RenderSystem.h>
#include <mpp/ResourceManager.h>
#include <mpp/helper/LineBatchDataProvider.h>
#include <mpp/helper/LineBatchRenderer.h>
#include <mpp/helper/TriangleBatchDataProvider.h>
#include <mpp/helper/TriangleBatchRenderer.h>
#include <mpp/mesh/VertexTypeSpecification.h>
#include <willpower/application/resourcesystem/ImageResource.h>
#include <willpower/application/resourcesystem/ImageSetResource.h>
#include <willpower/application/resourcesystem/ResourceManager.h>

namespace pf::application::render {
namespace resources = wp::application::resourcesystem;
namespace {
	using Float = mpp::mesh::DataTypeFloat;
	using Byte = mpp::mesh::DataTypeUnsignedByte;

	bool same(Vec2 left, Vec2 right)
	{
		return left.x == right.x && left.y == right.y;
	}

	bool same(WorldDrawList::ClipRectangle const& left,
		WorldDrawList::ClipRectangle const& right)
	{
		return same(left.minimum, right.minimum) && same(left.maximum, right.maximum);
	}

	void bytes(PackedColourValue value, uint8_t& red, uint8_t& green, uint8_t& blue,
		uint8_t& alpha)
	{
		red = static_cast<uint8_t>((value >> RedShift) & 0xff);
		green = static_cast<uint8_t>((value >> GreenShift) & 0xff);
		blue = static_cast<uint8_t>((value >> BlueShift) & 0xff);
		alpha = static_cast<uint8_t>((value >> AlphaShift) & 0xff);
	}

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
		enum class Kind { Triangles, Lines } kind{};
		WorldDrawList::Texture texture{ WorldDrawList::Texture::None };
		float lineWidth{ 1.0f };
		WorldDrawList::ClipRectangle clip{};
		std::vector<WorldDrawList::Triangle> triangles;
		std::vector<WorldDrawList::Line> lines;
	};

}

class WorldRenderer::Impl {
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
    Impl(resources::ResourceManager& resources, mpp::RenderSystem& renderSystem,
         mpp::ResourceManager& renderResources, mpp::ScenePtr scene,
         std::shared_ptr<const core::World> world)
        : mResources(resources), mRenderSystem(&renderSystem),
          mRenderResources(&renderResources), mScene(std::move(scene)), mWorld(std::move(world)) {
        try {
            mSectorName = mWorld->sectorImageSetResourceName();
            try {
                auto candidate = mResources.getResource(mSectorName);
                if (std::dynamic_pointer_cast<resources::ImageSetResource>(candidate)
                    && mResources.isResourceLoaded(candidate)) {
                    mResources.acquireResource(candidate);
                    mSectorSet = candidate;
                }
            } catch (std::exception const& error) {
                std::cerr << "Sector ImageSet unavailable: " << error.what() << '\n';
            }
            if (!mSectorSet)
                std::cerr << "Sector ImageSet unavailable: " << mSectorName << '\n';
            mAgentName = mWorld->agentImageSetResourceName();
            try {
                auto candidate = mResources.getResource(mAgentName);
                if (std::dynamic_pointer_cast<resources::ImageSetResource>(candidate)
                    && mResources.isResourceLoaded(candidate)) {
                    mResources.acquireResource(candidate);
                    mAgentSet = candidate;
                }
            } catch (std::exception const& error) {
                std::cerr << "Agent ImageSet unavailable: " << error.what() << '\n';
            }
            if (!mAgentSet) std::cerr << "Agent ImageSet unavailable: " << mAgentName << '\n';
            mFurnitureName = mWorld->furnitureImageSetResourceName();
            try {
                auto candidate = mResources.getResource(mFurnitureName);
                if (std::dynamic_pointer_cast<resources::ImageSetResource>(candidate)
                    && mResources.isResourceLoaded(candidate)) {
                    mResources.acquireResource(candidate);
                    mFurnitureSet = candidate;
                }
            } catch (std::exception const& error) {
                std::cerr << "Furniture ImageSet unavailable: " << error.what() << '\n';
            }
            if (!mFurnitureSet) std::cerr << "Furniture ImageSet unavailable: " << mFurnitureName << '\n';
            mObjectSet = mResources.acquireResource("ObjectAtlas");
            for (auto const& set : {mSectorSet, mObjectSet, mAgentSet, mFurnitureSet}) {
                if (!set) continue;
                if (!std::dynamic_pointer_cast<resources::ImageSetResource>(set))
                    throw std::runtime_error("Application atlas must be an ImageSet");
                if (!mResources.isResourceLoaded(set) ||
                    !mResources.isResourceLoaded(
                        std::static_pointer_cast<resources::ImageSetResource>(set)->getImage()))
                    throw std::runtime_error("Application atlases must be loaded by MapLoad");
            }
            installAtlasDefinitions();
        } catch (...) { cleanup(); throw; }
    }
    ~Impl() { cleanup(); }
    void render() {
        if (mSectorName != mWorld->sectorImageSetResourceName()) {
            mSectorName = mWorld->sectorImageSetResourceName();
            for (auto& slot : mSlots) remove(slot);
            mSlots.clear();
            mSlotAllocator = {};
            if (mSectorSet) mResources.releaseResource(mSectorSet);
            mSectorSet.reset();
            resources::ResourcePtr candidate;
            try {
                candidate = mResources.getResource(mSectorName);
                if (!std::dynamic_pointer_cast<resources::ImageSetResource>(candidate))
                    throw std::runtime_error("Selected sector resource is not an ImageSet");
                mResources.acquireResource(candidate);
                mResources.createResource(candidate);
                mResources.loadResource(candidate);
                mSectorSet = candidate;
            } catch (std::exception const& error) {
                if (candidate && std::dynamic_pointer_cast<resources::ImageSetResource>(candidate))
                    mResources.releaseResource(candidate);
                std::cerr << "Sector ImageSet unavailable: " << error.what() << '\n';
            }
            installAtlasDefinitions();
        }
        if (mAgentName != mWorld->agentImageSetResourceName()) {
            mAgentName = mWorld->agentImageSetResourceName();
            for (auto& slot : mSlots) remove(slot);
            mSlots.clear(); mSlotAllocator = {};
            if (mAgentSet) mResources.releaseResource(mAgentSet);
            mAgentSet.reset();
            resources::ResourcePtr candidate;
            try {
                candidate = mResources.getResource(mAgentName);
                if (!std::dynamic_pointer_cast<resources::ImageSetResource>(candidate))
                    throw std::runtime_error("Selected Agent resource is not an ImageSet");
                mResources.acquireResource(candidate);
                mResources.createResource(candidate);
                mResources.loadResource(candidate);
                mAgentSet = candidate;
            } catch (std::exception const& error) {
                if (candidate && std::dynamic_pointer_cast<resources::ImageSetResource>(candidate))
                    mResources.releaseResource(candidate);
                std::cerr << "Agent ImageSet unavailable: " << error.what() << '\n';
            }
            installAtlasDefinitions();
        }
        if (mFurnitureName != mWorld->furnitureImageSetResourceName()) {
            mFurnitureName = mWorld->furnitureImageSetResourceName();
            for (auto& slot : mSlots) remove(slot);
            mSlots.clear(); mSlotAllocator = {};
            if (mFurnitureSet) mResources.releaseResource(mFurnitureSet);
            mFurnitureSet.reset();
            resources::ResourcePtr candidate;
            try {
                candidate = mResources.getResource(mFurnitureName);
                if (!std::dynamic_pointer_cast<resources::ImageSetResource>(candidate))
                    throw std::runtime_error("Selected Furniture resource is not an ImageSet");
                mResources.acquireResource(candidate);
                mResources.createResource(candidate);
                mResources.loadResource(candidate);
                mFurnitureSet = candidate;
            } catch (std::exception const& error) {
                if (candidate && std::dynamic_pointer_cast<resources::ImageSetResource>(candidate))
                    mResources.releaseResource(candidate);
                std::cerr << "Furniture ImageSet unavailable: " << error.what() << '\n';
            }
            installAtlasDefinitions();
        }
        if (mFurnitureSet && mWorld->furnitureCatalogue())
            for (auto const& [key, definition] : mWorld->furnitureCatalogue()->definitions())
                for (auto const& tile : definition.tiles)
                    if (!furnitureSpriteSize(tile.image.c_str())
                        && mMissingFurnitureTiles.insert(mFurnitureName + "/" + tile.image).second)
                        std::cerr << "Furniture ImageSet " << mFurnitureName << " lacks tile " << tile.image
                                  << "; rendering in magenta\n";
        auto const width = mRenderSystem->getWindowWidth();
        auto const height = mRenderSystem->getWindowHeight();
        if (!width || !height) return;
        if (mAgentSet) for (auto const& snapshot : mWorld->getSimulationSnapshotView().agents) {
            auto agent = mWorld->lookupAgent(snapshot.id).entity;
            if (!agent) continue;
            for (auto const& tile : {agent->getPoseImageTile(),
                     agent->getPhysicalBaseline().poses.at(core::Pose::Standing).imageTile})
                if (!agentSpriteSize(tile.c_str()) && mMissingAgentTiles.insert(mAgentName + "/" + tile).second)
                    std::cerr << "Agent ImageSet " << mAgentName << " lacks required tile " << tile
                              << "; rendering physical bounds in magenta\n";
        }
        auto commands = drawWorld(mWorld, static_cast<float>(width), static_cast<float>(height));
        auto segments = buildSegments(commands);
        prepareSlots(segments, {0, 0});
        mRenderSystem->pushProjectionMatrix();
        mRenderSystem->pushModelMatrix();
        mRenderSystem->setProjection2dOrthographic();
        mRenderSystem->resetTransform();
        auto const restore = [&] {
            mRenderSystem->popModelMatrix();
            mRenderSystem->popProjectionMatrix();
            setNativeLineWidth(*mRenderSystem, 1.0f);
            for (auto& slot : mSlots) slot.sceneModel->setVisible(false);
        };
        try {
            for (std::size_t i = 0; i < segments.size(); ++i) {
                auto const& segment = segments[i];
                auto clip = targetClip(segment.clip, {0, 0}, static_cast<int>(width), static_cast<int>(height));
                if (!clip.width || !clip.height) continue;
                mRenderSystem->pushClipRectangle(clip);
                try {
                    if (segment.kind == Segment::Kind::Lines) setNativeLineWidth(*mRenderSystem, segment.lineWidth);
                    mSlots[mSegmentSlots[i]].sceneModel->render({});
                    // Flush while this segment's aperture clip is still active.
                    mRenderSystem->flushVertexBuffers();
                } catch (...) { mRenderSystem->popClipRectangle(); throw; }
                mRenderSystem->popClipRectangle();
            }
        } catch (...) { restore(); throw; }
        restore();
    }
private:
    void cleanup() noexcept {
        for (auto& slot : mSlots) remove(slot);
        mSlots.clear();
        clearSectorTileset(); clearObjectTileset(); clearAgentTileset(); clearFurnitureTileset();
        for (auto& set : {&mFurnitureSet, &mAgentSet, &mObjectSet, &mSectorSet}) {
            if (!*set) continue;
            try {
                auto atlas = std::dynamic_pointer_cast<resources::ImageSetResource>(*set);
                auto image = atlas ? atlas->getImage() : resources::ResourcePtr{};
                mResources.releaseResource(*set);
                if (!mResources.isResourceLoaded(*set)) mResources.destroyResources({*set});
                if (image && !mResources.isResourceLoaded(image)) mResources.destroyResources({image});
            } catch (std::exception const& error) {
                std::cerr << "Application atlas cleanup: " << error.what() << '\n';
            }
            set->reset();
        }
    }
    mpp::ResourcePtr texture(WorldDrawList::Texture kind) {
        if (kind == WorldDrawList::Texture::None) return {};
        auto set = std::static_pointer_cast<resources::ImageSetResource>(
            kind == WorldDrawList::Texture::SectorAtlas ? mSectorSet
                : kind == WorldDrawList::Texture::AgentAtlas ? mAgentSet
                : kind == WorldDrawList::Texture::FurnitureAtlas ? mFurnitureSet : mObjectSet);
        return set->getImage()->getMppResource();
    }
		void installAtlasDefinitions()
		{
			auto sector = std::dynamic_pointer_cast<resources::ImageSetResource>(mSectorSet);
			SectorTileset sectorTiles;
			if (sector)
			{
				auto sectorImage = std::dynamic_pointer_cast<resources::ImageResource>(sector->getImage());
				sectorTiles.width = sectorImage->getWidth();
				sectorTiles.height = sectorImage->getHeight();
				for (auto const& [name, definition] : sector->getImageDefinitions())
				{
					SectorTileRegion region{ static_cast<int>(definition.x),
						static_cast<int>(definition.y), static_cast<int>(definition.width),
						static_cast<int>(definition.height) };
					if (name.rfind("boundary-", 0) == 0)
						sectorTiles.boundaries.emplace(name.substr(9), region);
					else sectorTiles.surfaces.emplace(name, region);
				}
			}
			setSectorTileset(std::move(sectorTiles));

			auto objects = std::dynamic_pointer_cast<resources::ImageSetResource>(mObjectSet);
			auto objectImage = std::dynamic_pointer_cast<resources::ImageResource>(objects->getImage());
			ObjectTileset objectTiles;
			objectTiles.width = objectImage->getWidth();
			objectTiles.height = objectImage->getHeight();
			for (auto const& [name, definition] : objects->getImageDefinitions())
			{
				SectorTileRegion region{ static_cast<int>(definition.x),
					static_cast<int>(definition.y), static_cast<int>(definition.width),
					static_cast<int>(definition.height) };
				auto const tintable = name == "agent" || name == "marker"
					|| name.starts_with("human-") || name == "robot-standing"
					|| name == "cleaning-bot-standing";
				objectTiles.sprites.emplace(name, ObjectSprite{ region, tintable });
			}
			setObjectTileset(std::move(objectTiles));
            ObjectTileset agentTiles;
            if (auto agents = std::dynamic_pointer_cast<resources::ImageSetResource>(mAgentSet)) {
                auto image = std::dynamic_pointer_cast<resources::ImageResource>(agents->getImage());
                agentTiles.width = image->getWidth(); agentTiles.height = image->getHeight();
                for (auto const& [name, definition] : agents->getImageDefinitions())
                    agentTiles.sprites.emplace(name, ObjectSprite{{int(definition.x), int(definition.y),
                        int(definition.width), int(definition.height)}, true});
            }
            setAgentTileset(std::move(agentTiles));
            ObjectTileset furnitureTiles;
            if (auto furniture = std::dynamic_pointer_cast<resources::ImageSetResource>(mFurnitureSet)) {
                auto image = std::dynamic_pointer_cast<resources::ImageResource>(furniture->getImage());
                furnitureTiles.width = image->getWidth(); furnitureTiles.height = image->getHeight();
                for (auto const& [name, definition] : furniture->getImageDefinitions())
                    furnitureTiles.sprites.emplace(name, ObjectSprite{{int(definition.x), int(definition.y),
                        int(definition.width), int(definition.height)}, false});
            }
            setFurnitureTileset(std::move(furnitureTiles));
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
			}
			std::erase_if(result, [](Segment const& segment)
				{ return segment.triangles.empty() && segment.lines.empty(); });
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
			auto const name = "Application.World." + std::to_string(order);
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
    resources::ResourceManager& mResources;
    mpp::RenderSystem* mRenderSystem;
    mpp::ResourceManager* mRenderResources;
    mpp::ScenePtr mScene;
    std::shared_ptr<const core::World> mWorld;
    resources::ResourcePtr mSectorSet, mObjectSet, mAgentSet, mFurnitureSet;
    std::string mSectorName, mAgentName, mFurnitureName;
    std::set<std::string> mMissingAgentTiles, mMissingFurnitureTiles;
    WorldRenderSlotAllocator mSlotAllocator;
    std::vector<Slot> mSlots;
    std::vector<std::size_t> mSegmentSlots;
};
WorldRenderer::WorldRenderer(resources::ResourceManager& resources, mpp::RenderSystem& renderSystem,
                             mpp::ResourceManager& renderResources, mpp::ScenePtr scene,
                             std::shared_ptr<const core::World> world)
    : mImpl(std::make_unique<Impl>(resources, renderSystem, renderResources, std::move(scene), std::move(world))) {}
WorldRenderer::~WorldRenderer() = default;
void WorldRenderer::render() { mImpl->render(); }
} // namespace pf::application::render
