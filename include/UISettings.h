#pragma once


struct UISettings
{
	enum Style
	{
		Light,
		Dark,
		Classic
	};

	enum SelectionMode
	{
		Object,
		Vertex,
		Sector
	};

	Style style{ Style::Dark };

	SelectionMode selectionMode{ SelectionMode::Object };

	int visibleLayer{ 0 };

	bool renderGrid{ false };

	bool renderGraph{ false };

	// The viewport always draws the selected Layer solid. This controls whether the
	// Layer directly behind it is also drawn, as a wireframe overlay. Every other
	// Layer is never drawn.
	bool renderNextLayerWireframe{ true };

	bool highlightNearestVertex{ false };

	bool renderAgentDebug{ true };

	// Scale multiplier used to present the World canvas. Values greater than one
	// zoom in; editor chrome is not scaled.
	float worldZoom{ 1.0f };

	float xOffset{ 32 };

	float yOffset{ 32 };

	// Screen-space rectangle occupied by the docked world view.
	float worldViewportX{ 0 };
	float worldViewportY{ 0 };
	float worldViewportWidth{ 0 };
	float worldViewportHeight{ 0 };

	bool worldPaused{ false };
	bool metricsEnabled{ false };
	int metricsPort{ 9464 };
};
