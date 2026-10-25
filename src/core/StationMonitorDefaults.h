#pragma once

#include "core/StationSystems.h"
#include <algorithm>

namespace core::monitor_defaults
{
// Generated IDs retain their template suffix across subject rename. Match it
// only within the current subject, never by ID alone: the old name may now
// belong to another subject. Rules, enabled flags and provenance are mutable
// authored data and must not participate in template identity.
inline bool sameTemplate(StationMonitorSettings const& monitor,
	StationMonitorSettings const& candidate, std::string const& suffix)
{
	return monitor.subjectType == candidate.subjectType && monitor.subjectId == candidate.subjectId
		&& monitor.id.ends_with(suffix);
}

inline void appendUnique(std::vector<StationMonitorSettings>& monitors,
	StationMonitorSettings candidate, std::string const& collisionPrefix = "default:")
{
	auto const base = candidate.id;
	unsigned serial = 2;
	while (std::any_of(monitors.begin(), monitors.end(), [&](auto const& m) { return m.id == candidate.id; }))
		candidate.id = collisionPrefix + std::to_string(serial++) + ":" + base;
	// Prefixing leaves the stable template suffix intact, including after
	// save/load, another rename, or another collision.
	monitors.push_back(std::move(candidate));
}

inline void apply(StationSystemsSettings& settings, StationMonitorSettings const& candidate)
{
	auto const prefix = candidate.subjectType == "atmosphere" ? candidate.subjectId + "-"
		: candidate.subjectType + "-" + candidate.subjectId + "-";
	auto const suffix = "-" + candidate.id.substr(prefix.size());
	if (std::none_of(settings.monitors.begin(), settings.monitors.end(),
		[&](auto const& monitor) { return sameTemplate(monitor, candidate, suffix); }))
		appendUnique(settings.monitors, candidate);
}
}
