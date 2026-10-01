#include <memory>
#include <stdexcept>
#include <string>

#include <yaml-cpp/yaml.h>

#include "AgentClipboard.h"
#include "DocumentEdit.h"
#include "TagsPanel.h"
#include "core/Agent.h"
#include "core/AgentTagRegistry.h"
#include "core/SerializationWorkData.h"
#include "core/World.h"
#include "core/YamlSerializer.h"

void runPermissionAdherenceSmokeChecks();

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	std::string serialize(core::Serializable const& document)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		document.serialize(*writer, work);
		writer->serialize();
		return writer->getSerializedString();
	}

	void deserialize(core::Serializable& document, std::string const& yaml)
	{
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		core::SerializationWorkData work;
		require(document.deserialize(*reader, work), "Permission adherence fixture failed to load");
	}

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

	void defaultsOverridesAndCompatibility()
	{
		Fixture f;
		require(core::agentPropertyMetadata(core::AgentPropertyType::PermissionAdherence)
			.propertyNamespace == "Pathing", "Permission adherence is not a Pathing property");
		auto effective = f.agent()->getEffectivePermissionAdherence();
		require(effective.value && !effective.individual && !effective.sourceTag,
			"Permission adherence did not default to true");
		require(f.registry->addAgentTagPermissionAdherence(f.tag, &f.diagnostic), f.diagnostic);
		require(f.registry->setAgentTagPermissionAdherence(f.tag, false, &f.diagnostic), f.diagnostic);
		effective = f.agent()->getEffectivePermissionAdherence();
		require(!effective.value && effective.sourceTag == f.tag && !effective.individual
			&& effective.propertyRevision == f.registry->getAgentTagPermissionAdherence(f.tag)->revision,
			"Tag Permission adherence lost value or source");
		require(f.world->setAgentIndividualPermissionAdherence(f.id, true, &f.diagnostic), f.diagnostic);
		effective = f.agent()->getEffectivePermissionAdherence();
		require(effective.value && effective.individual && !effective.sourceTag,
			"Individual Permission adherence did not override the tag");
		require(f.world->setAgentIndividualPermissionAdherence(f.id, std::nullopt, &f.diagnostic), f.diagnostic);
		require(!f.agent()->getEffectivePermissionAdherence().value,
			"Removing the individual override did not reveal the tag value");

		auto other = f.registry->addAgentTag("visitors");
		require(f.world->assignAgentTag(f.id, other, &f.diagnostic), f.diagnostic);
		require(!f.registry->addAgentTagPermissionAdherence(other, &f.diagnostic),
			"Registry accepted conflicting inherited Permission adherence");
		require(f.world->removeAgentTag(f.id, other, &f.diagnostic), f.diagnostic);
		require(f.registry->addAgentTagPermissionAdherence(other, &f.diagnostic), f.diagnostic);
		require(!f.world->assignAgentTag(f.id, other, &f.diagnostic),
			"Assignment accepted conflicting inherited Permission adherence");
		require(f.world->resumeSimulation(), "Permission adherence fixture could not resume");
		require(!f.world->setAgentIndividualPermissionAdherence(f.id, true, &f.diagnostic)
			&& !f.registry->setAgentTagPermissionAdherence(f.tag, true, &f.diagnostic),
			"Permission adherence editing was allowed while simulation was running");
	}

	void persistenceCopyAndLegacyDefaults()
	{
		Fixture f;
		auto legacyRegistry = YAML::Load(serialize(*f.registry));
		legacyRegistry["agentTagRegistry"]["version"] = 13;
		auto legacyWorld = YAML::Load(serialize(*f.world));
		legacyWorld["world"]["version"] = 30;

		require(f.registry->addAgentTagPermissionAdherence(f.tag, &f.diagnostic), f.diagnostic);
		require(f.registry->setAgentTagPermissionAdherence(f.tag, false, &f.diagnostic), f.diagnostic);
		require(f.world->setAgentIndividualPermissionAdherence(f.id, false, &f.diagnostic), f.diagnostic);

		auto registryCopy = core::AgentTagRegistry::create();
		deserialize(*registryCopy, serialize(*f.registry));
		auto loaded = std::make_shared<core::World>("Loaded", 1, 1);
		deserialize(*loaded, serialize(*f.world));
		loaded->resolveAgentTagRegistry(registryCopy);
		auto loadedAgent = loaded->lookupAgent(f.id).entity;
		require(registryCopy->getAgentTagPermissionAdherence(f.tag)
			&& !registryCopy->getAgentTagPermissionAdherence(f.tag)->value
			&& loadedAgent->getIndividualPermissionAdherence() == false
			&& !loadedAgent->getEffectivePermissionAdherence().value,
			"Permission adherence did not survive World and registry round trips");

		auto legacyRegistryCopy = core::AgentTagRegistry::create();
		deserialize(*legacyRegistryCopy, YAML::Dump(legacyRegistry));
		deserialize(*loaded, YAML::Dump(legacyWorld));
		loaded->resolveAgentTagRegistry(legacyRegistryCopy);
		require(loaded->lookupAgent(f.id).entity->getEffectivePermissionAdherence().value,
			"Legacy documents did not receive default-true Permission adherence");
	}

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
		require(readAgentClipboardObject(YAML::Load(text)["prometheumClipboard"]["object"],
			payload, f.diagnostic), f.diagnostic);
		require(payload.individualPermissionAdherence == false,
			"Clipboard lost an explicit false Permission adherence value");
		core::AgentId pastedId;
		gWorldDocumentHistory.clear();
		require(commitAgentPlacement(f.world, payload, f.world->getSector(f.corridor),
			0, 4, pastedId, f.diagnostic), f.diagnostic);
		require(f.world->lookupAgent(pastedId).entity->getIndividualPermissionAdherence() == false,
			"Paste did not preserve explicit false Permission adherence");

		auto legacy = YAML::Load(text)["prometheumClipboard"]["object"];
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

void runPermissionAdherenceSmokeChecks()
{
	defaultsOverridesAndCompatibility();
	persistenceCopyAndLegacyDefaults();
	historyAndClipboard();
}
