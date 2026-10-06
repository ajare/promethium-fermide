#include "Checks.h"
#include "EditorState.h"
#include <limits>
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
		require(document.deserialize(*reader, work), "Route planning time fixture failed to load");
	}

	struct Fixture
	{
		std::shared_ptr<core::AgentTagRegistry> registry{ core::AgentTagRegistry::create() };
		std::shared_ptr<core::World> world{ std::make_shared<core::World>("Planning times", 10, 2) };
		core::AgentTagId tag{ registry->addAgentTag("planner") };
		uint32_t corridor{};
		core::AgentId id{};
		std::string diagnostic;

		Fixture()
		{
			world->attachAgentTagRegistry("planning.tags.yaml", registry);
			corridor = world->addCorridor(0, 0, 9);
			world->finishBuild();
			world->pauseSimulation();
			id = world->createAgent("Planner", corridor, 0, 1.5f);
			require(world->assignAgentTag(id, tag, &diagnostic), diagnostic);
		}

		~Fixture() { forgetAgentTagRegistryDocument(registry); }
		core::Agent* agent() const { return world->lookupAgent(id).entity; }

		void addProperties()
		{
			require(registry->addAgentTagMinimumRoutePlanningTime(tag, &diagnostic), diagnostic);
			require(registry->addAgentTagMaximumRoutePlanningTime(tag, &diagnostic), diagnostic);
		}
	};

	void individualHistory()
	{
		Fixture f;
		f.addProperties();
		f.world->markSaved();
		DocumentHistory history;
		history.markSaved();
		auto before = captureDocumentSnapshot(f.world, history);
		require(f.world->setAgentIndividualMinimumRoutePlanningTime(f.id, 8, &f.diagnostic), f.diagnostic);
		require(f.world->setAgentIndividualMaximumRoutePlanningTime(f.id, 2, &f.diagnostic), f.diagnostic);
		commitDocumentEdit(std::move(before), history);
		require(history.isModified() && f.world->isModified(), "Individual planning times did not dirty the World");
		auto restore = [&](DocumentSnapshot const& snapshot)
		{
			deserialize(*f.world, snapshot.yaml);
			f.world->pauseSimulation();
			f.world->resolveAgentTagRegistry(f.registry);
			return true;
		};
		require(history.undo(captureDocumentSnapshot(f.world, history), restore), "Individual planning time undo failed");
		require(!history.isModified() && !f.agent()->getIndividualMinimumRoutePlanningTime()
			&& !f.agent()->getIndividualMaximumRoutePlanningTime()
			&& f.agent()->getEffectiveMinimumRoutePlanningTime().value == 1
			&& f.agent()->getEffectiveMaximumRoutePlanningTime().value == 3,
			"Individual undo did not reveal inherited defaults");
		require(history.redo(captureDocumentSnapshot(f.world, history), restore), "Individual planning time redo failed");
		require(history.isModified() && f.agent()->getIndividualMinimumRoutePlanningTime() == 8
			&& f.agent()->getIndividualMaximumRoutePlanningTime() == 2
			&& f.agent()->getEffectiveMaximumRoutePlanningTime().value == 8,
			"Individual redo rewrote crossed authored values");
	}

	void historyAndClipboard()
	{
		Fixture f;
		f.addProperties();
		f.world->markSaved();
		f.registry->markUnmodified();
		auto const oldMinimum = f.agent()->getMinimumRoutePlanningTimeSample();
		auto const oldMaximum = f.agent()->getMaximumRoutePlanningTimeSample();
		require(commitAgentTagMinimumRoutePlanningTimeEdit(f.registry, f.tag, { 6, 8 }, f.diagnostic), f.diagnostic);
		require(commitAgentTagMaximumRoutePlanningTimeEdit(f.registry, f.tag, { 2, 4 }, f.diagnostic), f.diagnostic);
		auto const minimum = f.agent()->getMinimumRoutePlanningTimeSample();
		auto const maximum = f.agent()->getMaximumRoutePlanningTimeSample();
		require(f.world->isModified() && f.registry->isModified(), "Registry edits did not dirty dependent documents");
		require(restoreAgentTagRegistrySnapshot(f.registry, false, &f.diagnostic), f.diagnostic);
		require(f.agent()->getMaximumRoutePlanningTimeSample() == oldMaximum
			&& f.agent()->getMinimumRoutePlanningTimeSample() == minimum, "Maximum undo changed minimum");
		require(restoreAgentTagRegistrySnapshot(f.registry, false, &f.diagnostic), f.diagnostic);
		require(f.agent()->getMinimumRoutePlanningTimeSample() == oldMinimum && !f.world->isModified(),
			"Minimum undo lost exact sample or clean state");
		require(restoreAgentTagRegistrySnapshot(f.registry, true, &f.diagnostic), f.diagnostic);
		require(restoreAgentTagRegistrySnapshot(f.registry, true, &f.diagnostic), f.diagnostic);
		require(f.agent()->getMinimumRoutePlanningTimeSample() == minimum
			&& f.agent()->getMaximumRoutePlanningTimeSample() == maximum, "Redo rerolled planning time samples");

		require(f.world->setAgentIndividualMinimumRoutePlanningTime(f.id, 9, &f.diagnostic), f.diagnostic);
		require(f.world->setAgentIndividualMaximumRoutePlanningTime(f.id, 1, &f.diagnostic), f.diagnostic);
		auto const text = makeAgentClipboardText(makeAgentClipboardPayload(*f.world, f.id, "Copy"), false);
		AgentClipboardPayload payload;
		require(readAgentClipboardObject(YAML::Load(text)["promethiumClipboard"]["object"], payload, f.diagnostic), f.diagnostic);
		require(payload.minimumRoutePlanningTimeSample == minimum && payload.maximumRoutePlanningTimeSample == maximum
			&& payload.individualMinimumRoutePlanningTime == 9 && payload.individualMaximumRoutePlanningTime == 1,
			"Clipboard lost raw individual values or sample provenance");
		// Preflight rejects stale provenance and invalid scalar values without mutation.
		auto invalid = payload;
		invalid.minimumRoutePlanningTimeSample->propertyRevision += 100;
		core::AgentId copy;
		auto const beforeInvalidPaste = serialize(*f.world);
		require(!commitAgentPlacement(f.world, invalid, f.world->getSector(f.corridor), 0, 4, copy, f.diagnostic)
			&& serialize(*f.world) == beforeInvalidPaste, "Paste accepted stale planning time provenance");
		invalid = payload;
		invalid.maximumRoutePlanningTimeSample->value = 11;
		require(!commitAgentPlacement(f.world, invalid, f.world->getSector(f.corridor), 0, 4, copy, f.diagnostic)
			&& serialize(*f.world) == beforeInvalidPaste, "Paste accepted an invalid planning time sample");
		invalid = payload;
		invalid.individualMaximumRoutePlanningTime = std::numeric_limits<float>::quiet_NaN();
		require(!commitAgentPlacement(f.world, invalid, f.world->getSector(f.corridor), 0, 4, copy, f.diagnostic)
			&& serialize(*f.world) == beforeInvalidPaste, "Paste accepted a nonfinite planning time");
		gWorldDocumentHistory.clear();
		require(commitAgentPlacement(f.world, payload, f.world->getSector(f.corridor), 0, 4, copy, f.diagnostic), f.diagnostic);
		auto const* pasted = f.world->lookupAgent(copy).entity;
		require(pasted->getMinimumRoutePlanningTimeSample() == minimum
			&& pasted->getMaximumRoutePlanningTimeSample() == maximum
			&& pasted->getIndividualMinimumRoutePlanningTime() == 9
			&& pasted->getIndividualMaximumRoutePlanningTime() == 1
			&& pasted->getEffectiveMaximumRoutePlanningTime().value == 9,
			"Paste did not preserve crossed authored values and inherited samples");
		// An old, untagged clipboard object receives defaults rather than authored overrides.
		auto legacy = YAML::Load(text)["promethiumClipboard"]["object"];
		legacy.remove("agentTagRegistryUuid");
		legacy.remove("tags");
		legacy.remove("propertySamples");
		legacy.remove("minimumRoutePlanningTime");
		legacy.remove("maximumRoutePlanningTime");
		legacy["name"] = "Legacy";
		require(readAgentClipboardObject(legacy, payload, f.diagnostic), f.diagnostic);
		require(commitAgentPlacement(f.world, payload, f.world->getSector(f.corridor), 0, 6, copy, f.diagnostic), f.diagnostic);
		pasted = f.world->lookupAgent(copy).entity;
		require(pasted->getEffectiveMinimumRoutePlanningTime().value == 1
			&& pasted->getEffectiveMaximumRoutePlanningTime().value == 3,
			"Legacy clipboard did not receive default planning times");
		gWorldDocumentHistory.clear();
	}}

namespace routing_smoke
{
	void registerPlanningTimeEditor(std::vector<smoke::Check>& checks)
	{
		checks.push_back({ "routing/individualHistory", [](smoke::Context const&)
		{
			EditorState state;
			individualHistory();
		} });
		checks.push_back({ "routing/historyAndClipboard", [](smoke::Context const&)
		{
			EditorState state;
			historyAndClipboard();
		} });
	}
}
