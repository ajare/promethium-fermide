#pragma once

#include <algorithm>
#include <cstddef>
#include "core/StationProtection.h"
#include <limits>
#include <vector>

namespace core::station_detail
{
	// Transaction-local pre-commit protection. The ledger holds each individual
	// accepted rate over an integration interval. Its positive and negative
	// parts bound excursions without assuming that simultaneous cooling cancels
	// heating (or that a later interval undoes an earlier excursion).
	struct ProtectionPrediction
	{
		enum class Body { Reservoir, Atmosphere, Loop };
		static constexpr double unbounded = std::numeric_limits<double>::infinity();
		struct Gate
		{
			Body body;
			size_t index;
			double minimum, maximum;
			bool operator==(Gate const&) const = default;
		};
		struct Range
		{
			Body body;
			size_t index;
			double minimum, maximum;
		};
		bool active{};
		std::vector<Gate> used, refused, heaters, heating;
		std::vector<Range> ranges;
		void begin() { used.clear(); heaters.clear(); ranges.clear(); }
		bool heatRequired(size_t index, double temperature, double setpoint)
		{
			if (!active) return temperature < setpoint;
			Gate gate{Body::Loop, index, setpoint, unbounded};
			if (std::find(heaters.begin(), heaters.end(), gate) == heaters.end()) heaters.push_back(gate);
			return temperature < setpoint || std::find(heating.begin(), heating.end(), gate) != heating.end();
		}
		bool qualifies(Body body, size_t index, double temperature,
			double minimum, double maximum)
		{
			if (temperature + StationProtectionTemperatureOvershootC < minimum
				|| temperature + StationProtectionTemperatureOvershootC >= maximum) return false;
			if (!active) return true;
			Gate gate{body, index, minimum, maximum};
			if (std::find(refused.begin(), refused.end(), gate) != refused.end()) return false;
			if (std::find(used.begin(), used.end(), gate) == used.end()) used.push_back(gate);
			return true;
		}
		void include(Body body, size_t index, double start, double finish,
			double capacity, double duration, std::vector<double> const& rates)
		{
			if (!active || capacity <= 0.0) return;
			double heating = 0.0, cooling = 0.0;
			for (auto rate : rates)
			{
				heating += std::max(0.0, rate);
				cooling += std::max(0.0, -rate);
			}
			auto const minimum = std::min(finish, start - cooling * duration / capacity);
			auto const maximum = std::max(finish, start + heating * duration / capacity);
			auto const found = std::find_if(ranges.begin(), ranges.end(), [&](Range const& range)
				{ return range.body == body && range.index == index; });
			if (found == ranges.end()) ranges.push_back({body, index, minimum, maximum});
			else
			{
				found->minimum = std::min(found->minimum, minimum);
				found->maximum = std::max(found->maximum, maximum);
			}
		}
		bool refine()
		{
			bool changed = false;
			for (auto const& gate : used)
				if (std::any_of(ranges.begin(), ranges.end(), [&](Range const& range)
				{
					return range.body == gate.body && range.index == gate.index
						&& (range.minimum + StationProtectionTemperatureOvershootC < gate.minimum
							|| range.maximum + StationProtectionTemperatureOvershootC >= gate.maximum);
				}))
				{
					refused.push_back(gate);
					changed = true;
				}
			for (auto const& heater : heaters)
				if (std::find(heating.begin(), heating.end(), heater) == heating.end()
					&& std::any_of(ranges.begin(), ranges.end(), [&](Range const& range)
					{
						return range.body == Body::Loop && range.index == heater.index && range.minimum < heater.minimum;
					}))
				{
					heating.push_back(heater);
					changed = true;
				}
			return changed;
		}
	};
}
