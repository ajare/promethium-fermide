#include "StationReferenceFixtures.h"
#include <fstream>
#include <iterator>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace core
{
namespace
{
std::string readStationFixture(std::string const& name)
{
	auto path = std::filesystem::path(PF_SMOKE_SOURCE_ROOT) / "resources/station-systems" / name;
	std::ifstream input(path, std::ios::binary);
	if (!input) throw std::runtime_error("Could not open Station fixture: " + path.string());
	std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	if (input.bad()) throw std::runtime_error("Could not read Station fixture: " + path.string());
	return text;
}
}

	std::string const& bundledStationReferenceText()
	{
		static auto const text = readStationFixture("reference-station.yaml");
		return text;
	}

	std::vector<BundledStationScenario> const& bundledStationScenarios()
	{
		// #423: the packaged emergency/recovery variants are parsed once,
		// each through the same authoritative reader and validation the
		// base reference passes - a scenario resource that does not parse
		// is a build defect, never a user error.
		static std::vector<BundledStationScenario> const scenarios = []
		{
			std::vector<BundledStationScenario> parsed;
			for (auto const* id : {"resupply-delay", "power-cooling-outage",
				"waste-constrained", "maintenance-recovery"})
			{
				BundledStationScenario scenario;
				scenario.id = id;
				scenario.text = readStationFixture("scenarios/" + scenario.id + ".yaml");
				std::string diagnostic;
				if (!parseStationReference(scenario.text, scenario.document, &diagnostic))
					throw std::runtime_error("Bundled station scenario '" + scenario.id
						+ "' failed to parse: " + diagnostic);
				parsed.push_back(std::move(scenario));
			}
			return parsed;
		}();
		return scenarios;
	}

	StationReferenceDocument const& bundledStationReference()
	{
		static StationReferenceDocument const document = []
		{
			StationReferenceDocument parsed;
			std::string diagnostic;
			if (!parseStationReference(bundledStationReferenceText(), parsed, &diagnostic))
				throw std::runtime_error(
					"Bundled reference station failed to parse: " + diagnostic);
			return parsed;
		}();
		return document;
	}

	std::string const& bundledStationLongRunWorkloadText()
	{
		// #424: the packaged long-duration stability workload text.
		static auto const text = readStationFixture("long-run-workload.yaml");
		return text;
	}

	StationReferenceDocument const& bundledStationLongRunWorkload()
	{
		// Parsed once through the same authoritative reader and full
		// physical-consistency validation the base reference passes - a
		// workload resource that does not parse is a build defect, never
		// a user error.
		static StationReferenceDocument const document = []
		{
			StationReferenceDocument parsed;
			std::string diagnostic;
			if (!parseStationReference(bundledStationLongRunWorkloadText(), parsed, &diagnostic))
				throw std::runtime_error(
					"Bundled long-run stability workload failed to parse: " + diagnostic);
			return parsed;
		}();
		return document;
	}
}
