#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>

#include "Render.h"
#include "UISettings.h"
#include "WorldViewportZoom.h"
#include "core/World.h"

extern UISettings gUISettings;

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	bool near(float left, float right)
	{
		return std::abs(left - right) < 0.001f;
	}
}

void runViewportZoomSmokeChecks()
{
	auto const previousSettings = gUISettings;

	// Zoom is a multiplier: zooming out can make the complete World fit.
	auto zoomedOut = worldViewportLayout({ 500.0f, 400.0f },
		{ 1000.0f, 800.0f }, 0.25f, 20.0f, 20.0f);
	require(near(zoomedOut.worldSize.x, 250.0f)
		&& near(zoomedOut.worldSize.y, 200.0f),
		"0.25x did not scale the World dimensions down");
	require(!zoomedOut.horizontalScrollbar && !zoomedOut.verticalScrollbar,
		"zoomed-out World incorrectly retained scrollbars");

	// Both the scrollbar ranges and the canvas reduction use scaled dimensions.
	auto zoomedIn = worldViewportLayout({ 500.0f, 400.0f },
		{ 1000.0f, 800.0f }, 2.0f, 20.0f, 20.0f);
	require(zoomedIn.horizontalScrollbar && zoomedIn.verticalScrollbar,
		"2x World did not show both required scrollbars");
	require(near(zoomedIn.canvasSize.x, 480.0f)
		&& near(zoomedIn.canvasSize.y, 380.0f),
		"scrollbars did not reduce the zoomed canvas");
	require(near(zoomedIn.scrollMaximum.x, 1520.0f)
		&& near(zoomedIn.scrollMaximum.y, 1220.0f),
		"zoomed scrollbar ranges do not cover the scaled World");

	// Adding one scrollbar can require the other; resolve both before computing
	// either range.
	auto coupled = worldViewportLayout({ 500.0f, 400.0f },
		{ 500.0f, 401.0f }, 1.0f, 20.0f, 20.0f);
	require(coupled.horizontalScrollbar && coupled.verticalScrollbar,
		"one scrollbar did not induce the other at the viewport boundary");

	// Renderer culling converts the scaled screen-space viewport back to the
	// unscaled pixel coordinates consumed by World::getSectorsInBounds().
	auto world = std::make_shared<core::World>("Zoom culling", 4, 4);
	while (world->getLayerCount() < 2) world->addLayer();
	auto const ground = world->addRoom("Ground", 0, 0, 0, 2, 1);
	auto const upper = world->addRoom("Upper", 0, 2, 0, 2, 1);
	world->finishBuild();
	gUISettings = UISettings{};
	gUISettings.worldZoom = 2.0f;
	gUISettings.yOffset = -640.0f;
	gUISettings.worldViewportWidth = 320.0f;
	gUISettings.worldViewportHeight = 320.0f;
	auto const visible = viewportSectors(world, 0);
	require(std::find(visible.begin(), visible.end(), world->getSector(upper))
		!= visible.end(), "2x culling omitted the Sector inside the scaled viewport");
	require(std::find(visible.begin(), visible.end(), world->getSector(ground))
		== visible.end(), "2x culling retained a Sector below the scaled viewport");
	gUISettings = previousSettings;
}
