// Migrated from AgentActivationSmokeChecks.cpp (#285); editor dependency tier.
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <yaml-cpp/yaml.h>
#include "core/Agent.h"
#include "core/World.h"
#include "core/EntityId.h"
#include "core/Sector.h"
#include "core/Simulation.h"
#include "core/YamlSerializer.h"
#include "AgentClipboard.h"
#include "Checks.h"
#include "EditorState.h"

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	// One corridor, one marker at its far end, and two Agents at the near
	// end, each already walking toward the marker. The walk is what tells a
	// simulated Agent from a parked one.
	struct WalkFixture
	{
		core::World world;
		uint32_t corridor;
		uint32_t destinationIdentifier;
		core::AgentId first;
		core::AgentId second;

		WalkFixture()
			: world("Activation walk", 12, 3)
			, destinationIdentifier{ 0x41475231u }
		{
			corridor = world.addCorridor(0, 0, 8);
			world.addSectorMarker(corridor, 0, 7.5f, &destinationIdentifier);
			world.finishBuild();

			auto const destination = world.getGraph()->getVertexByIdentifier(destinationIdentifier);
			require(destination != nullptr, "The walk destination vertex is missing");

			first = world.createAgent("Walker", corridor, 0, 0.5f);
			second = world.createAgent("Parker", corridor, 0, 1.5f);
			for (auto id : { first, second })
			{
				auto* agent = world.lookupAgent(id).entity;
				auto path = world.getGraph()->calculatePath(agent, destination);
				require(path && !path->nodes.empty(), "The walk route could not be calculated");
				agent->setPath(std::move(path), true);
			}
		}
	};

	// The clipboard envelope around a hand-written `object` map body, the
	// same envelope makeAgentClipboardText writes.
	std::string clipboardTextWithObjectBody(std::string const& body)
	{
		return "promethiumClipboard:\n  version: 1\n  operation: copy\n  type: Agent\n"
			"  object:\n" + body;
	}

	void clipboardCarriesActivation()
	{
		WalkFixture fixture;
		std::string diagnostic;

		fixture.world.pauseSimulation();
		require(fixture.world.setAgentActive(fixture.second, false, &diagnostic),
			"The deactivation was refused: " + diagnostic);

		auto const payload = makeAgentClipboardPayload(fixture.world, fixture.second, "Parked copy");
		require(!payload.active, "A copied deactivated Agent's payload stayed activated");
		auto const text = makeAgentClipboardText(payload, false);
		require(text.find("active") != std::string::npos,
			"A deactivated Agent's clipboard text does not mention activation");

		auto const parsed = YAML::Load(text);
		AgentClipboardPayload read;
		require(readAgentClipboardObject(parsed["promethiumClipboard"]["object"], read, diagnostic),
			"The clipboard text did not parse: " + diagnostic);
		require(!read.active, "A parsed payload lost the deactivation");

		// No `active` key - the shape every payload written before activation
		// existed has - reads back activated.
		AgentClipboardPayload legacy{ "Legacy", 0, true, std::nullopt };
		auto const legacyText = makeAgentClipboardText(legacy, false);
		require(legacyText.find("active") == std::string::npos,
			"An activated Agent's clipboard text mentions activation");
		AgentClipboardPayload legacyRead;
		require(readAgentClipboardObject(
			YAML::Load(legacyText)["promethiumClipboard"]["object"], legacyRead, diagnostic),
			"The legacy clipboard text did not parse: " + diagnostic);
		require(legacyRead.active, "A payload without the activation key did not read as activated");

		// A present key of the wrong shape is refused, not coerced: a
		// silently-activated paste of a parked Agent would start simulating
		// someone the author had parked.
		auto const malformedText = clipboardTextWithObjectBody(
			"    name: Broken\n    flags: 0\n    active: [not, a, bool]\n");
		AgentClipboardPayload malformedRead;
		require(!readAgentClipboardObject(
			YAML::Load(malformedText)["promethiumClipboard"]["object"], malformedRead, diagnostic),
			"A non-boolean activation was read instead of refused");
		require(!diagnostic.empty(), "A refused clipboard payload gave no reason");
	}
}

void agent_smoke::registerActivationEditor(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "agent/clipboardCarriesActivation", [](smoke::Context const&) { EditorState state; clipboardCarriesActivation(); } });
}
