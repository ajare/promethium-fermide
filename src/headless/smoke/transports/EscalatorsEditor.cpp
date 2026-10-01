#include "../editor/Checks.h"
#include "../editor/State.h"
#include "Smoke.h"
#include "EscalatorFixture.h"
#include "TagsPanel.h"
#include "DocumentEdit.h"
#include <limits>
#include <yaml-cpp/yaml.h>

namespace
{
	void propertyWorkflows()
	{
		Fixture fixture(0);
		auto& registry = fixture.registry;
		auto tag = fixture.tag;
		std::string diagnostic;
		auto const before = serialize(*registry);
		for (float invalid : { -0.1f, 1.1f, std::numeric_limits<float>::infinity(),
			-std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() })
			require(!registry->setAgentTagEscalatorWalkingChance(tag, invalid, &diagnostic)
				&& !diagnostic.empty() && serialize(*registry) == before, "invalid chance changed state");
		for (auto invalid : { "-0.1", "1.1", ".inf", "-.inf", ".nan" })
		{
			auto malformed = YAML::Load(before);
			malformed["tags"][0]["properties"][0]["value"] = YAML::Load(invalid);
			bool refused = false;
			try { load(*registry, YAML::Dump(malformed)); }
			catch (std::exception const&) { refused = true; }
			require(refused && serialize(*registry) == before, "invalid persisted chance accepted");
		}
		{
			auto malformed = YAML::Load(before);
			malformed["tags"][0]["properties"].push_back(
				YAML::Clone(malformed["tags"][0]["properties"][0]));
			bool refused = false;
			try { load(*registry, YAML::Dump(malformed)); }
			catch (std::exception const&) { refused = true; }
			require(refused && serialize(*registry) == before, "duplicate chance accepted");
		}
		for (float value : { 1.0f, 0.0f })
		{
			require(commitAgentTagEscalatorWalkingChanceEdit(registry, tag, value, diagnostic), diagnostic);
			auto reopened = core::AgentTagRegistry::create();
			load(*reopened, serialize(*registry));
			require(reopened->getAgentTagEscalatorWalkingChance(tag)->value == value, "endpoint round-trip");
			require(core::AgentTagRegistry::copyWithNewUuid(*registry)->hasEquivalentDefinitions(*registry), "copy lost property");
			require(restoreAgentTagRegistrySnapshot(registry, false, &diagnostic), diagnostic);
			require(fixture.agent()->getEffectiveEscalatorWalkingChance().value == 1.0f - value, "undo value");
			require(restoreAgentTagRegistrySnapshot(registry, true, &diagnostic), diagnostic);
			require(fixture.agent()->getEffectiveEscalatorWalkingChance().value == value, "redo value");
		}
		auto const other = registry->addAgentTag("other");
		require(registry->addAgentTagEscalatorWalkingChance(other), "second property");
		require(!fixture.world.assignAgentTag(fixture.id, other, &diagnostic), "assignment conflict accepted");
		require(registry->removeAgentTagEscalatorWalkingChance(other)
			&& fixture.world.assignAgentTag(fixture.id, other), "assign empty tag");
		require(!registry->addAgentTagEscalatorWalkingChance(other, &diagnostic), "addition conflict accepted");
		auto replacement = core::AgentTagRegistry::create();
		load(*replacement, serialize(*registry));
		require(replacement->addAgentTagEscalatorWalkingChance(other), "replacement property");
		require(!registry->replaceDefinitionsFrom(std::move(*replacement), &diagnostic), "reload conflict accepted");
		require(commitAgentTagEscalatorWalkingChanceRemove(registry, tag, diagnostic), diagnostic);
		require(!fixture.agent()->getEffectiveEscalatorWalkingChance().sourceTag, "removal retained inheritance");
		require(restoreAgentTagRegistrySnapshot(registry, false, &diagnostic), diagnostic);
		require(fixture.agent()->getEffectiveEscalatorWalkingChance().sourceTag == tag, "undo removal");
		require(registry->deleteAgentTag(tag), "delete tag");
		require(fixture.agent()->getEffectiveEscalatorWalkingChance().value == 0, "delete default");
	}


	void properties(smoke::Context const&)
	{
		editor_smoke::State state;
		propertyWorkflows();
	}
}

void editor_smoke::registerEscalators(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "transports/propertyWorkflows", properties });
}
