#pragma once
#include "core/Path.h"
#include <memory>
#include <optional>
#include <utility>

namespace smoke
{
	inline std::shared_ptr<core::Path> twoNodePath(std::shared_ptr<const core::Vertex> source,
		std::shared_ptr<const core::Vertex> destination, std::shared_ptr<const core::Edge> edge)
	{
		auto path = std::make_shared<core::Path>();
		path->nodes.push_back({ nullptr, std::move(source), 0.0f, std::nullopt, std::nullopt });
		path->nodes.push_back({ std::move(edge), std::move(destination), 1.0f, std::nullopt, std::nullopt });
		return path;
	}
}
