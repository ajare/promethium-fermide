#include "Checks.h"
#include "EditorState.h"
#include "ImGuiContext.h"
#include <memory>
#include <stdexcept>
#include <string>

#include <yaml-cpp/yaml.h>

#include "AgentClipboard.h"
#include "DocumentEdit.h"
#include "TagsPanel.h"
#include "core/Agent.h"
#include "core/AgentTagRegistry.h"
#include "core/Edge.h"
#include "core/RouteCost.h"
#include "core/SerializationWorkData.h"
#include "core/World.h"
#include "core/YamlSerializer.h"

namespace
{
	void require(bool condition, std::string const& message)
	{ if (!condition) throw std::runtime_error(message); }

	struct Fixture
	{
		std::shared_ptr<core::AgentTagRegistry> registry{ core::AgentTagRegistry::create() };
		std::shared_ptr<core::World> world{ std::make_shared<core::World>("Adherence", 8, 2) };
		core::AgentTagId tag{ registry->addAgentTag("staff") };
		uint32_t corridor{};
		core::AgentId id{};
		std::string diagnostic;

		Fixture()
		{
			world->attachAgentTagRegistry("adherence.tags.yaml", registry);
			corridor = world->addCorridor(0, 0, 8);
			world->finishBuild();
			world->pauseSimulation();
			id = world->createAgent("Walker", corridor, 0, 1.5f);
			require(world->assignAgentTag(id, tag, &diagnostic), diagnostic);
		}

		~Fixture() { forgetAgentTagRegistryDocument(registry); }
		core::Agent* agent() const { return world->lookupAgent(id).entity; }
	};

	void historyAndClipboard()
	{
		Fixture f;
		f.world->markSaved();
		f.registry->markUnmodified();
		require(commitAgentTagPermissionAdherenceAdd(f.registry, f.tag, f.diagnostic), f.diagnostic);
		require(commitAgentTagPermissionAdherenceEdit(f.registry, f.tag, false, f.diagnostic), f.diagnostic);
		require(!f.agent()->getEffectivePermissionAdherence().value,
			"Registry editor workflow did not update Permission adherence");
		require(restoreAgentTagRegistrySnapshot(f.registry, false, &f.diagnostic), f.diagnostic);
		require(f.agent()->getEffectivePermissionAdherence().value,
			"Registry undo did not restore the prior Permission adherence value");
		require(restoreAgentTagRegistrySnapshot(f.registry, true, &f.diagnostic), f.diagnostic);
		require(!f.agent()->getEffectivePermissionAdherence().value,
			"Registry redo did not restore Permission adherence false");

		require(f.world->setAgentIndividualPermissionAdherence(f.id, false, &f.diagnostic), f.diagnostic);
		auto text = makeAgentClipboardText(makeAgentClipboardPayload(*f.world, f.id, "Copy"), false);
		AgentClipboardPayload payload;
		require(readAgentClipboardObject(YAML::Load(text)["promethiumClipboard"]["object"],
			payload, f.diagnostic), f.diagnostic);
		require(payload.individualPermissionAdherence == false,
			"Clipboard lost an explicit false Permission adherence value");
		core::AgentId pastedId;
		gWorldDocumentHistory.clear();
		require(commitAgentPlacement(f.world, payload, f.world->getSector(f.corridor),
			0, 4, pastedId, f.diagnostic), f.diagnostic);
		require(f.world->lookupAgent(pastedId).entity->getIndividualPermissionAdherence() == false,
			"Paste did not preserve explicit false Permission adherence");

		auto legacy = YAML::Load(text)["promethiumClipboard"]["object"];
		legacy.remove("permissionAdherence");
		legacy.remove("agentTagRegistryUuid");
		legacy.remove("tags");
		legacy["name"] = "Legacy";
		require(readAgentClipboardObject(legacy, payload, f.diagnostic), f.diagnostic);
		require(commitAgentPlacement(f.world, payload, f.world->getSector(f.corridor),
			0, 6, pastedId, f.diagnostic), f.diagnostic);
		require(f.world->lookupAgent(pastedId).entity->getEffectivePermissionAdherence().value,
			"Legacy clipboard payload did not receive default-true Permission adherence");
		gWorldDocumentHistory.clear();
	}
}

void permission_smoke::registerAdherenceEditor(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "permissions/historyAndClipboard",
		[](smoke::Context const&)
		{
			EditorState state;
			historyAndClipboard();
		} });
}
