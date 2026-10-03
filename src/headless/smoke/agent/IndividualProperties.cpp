// Migrated from AgentIndividualPropertySmokeChecks.cpp (#286); core dependency tier.
#include <memory>
#include <stdexcept>
#include <string>

#include "core/Agent.h"
#include "core/AgentTagRegistry.h"
#include "core/SerializationWorkData.h"
#include "core/MobilityProfile.h"
#include "core/World.h"
#include "core/YamlSerializer.h"


#include "Checks.h"

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
}

void individualPropertiesOverrideTagPropertiesAndPersist()
{
	core::RouteChoicePolicy persistencePolicy;
	persistencePolicy.minimumSwitchGainSeconds = 2.0f;
	require(!persistencePolicy.shouldReplacePath(100.0f, 84.0f, 0.20f)
		&& persistencePolicy.shouldReplacePath(100.0f, 79.0f, 0.20f)
		&& !persistencePolicy.shouldReplacePath(10.0f, 8.0f, 0.0f)
		&& persistencePolicy.shouldReplacePath(10.0f, 7.9f, 0.0f),
		"Route persistence did not require both strict absolute and proportional gains");

	auto const interactionMetadata = core::agentPropertyMetadata(
		core::AgentPropertyType::InteractionAversion);
	auto const effortMetadata = core::agentPropertyMetadata(
		core::AgentPropertyType::EffortAversion);
	auto const waitingMetadata = core::agentPropertyMetadata(
		core::AgentPropertyType::WaitingAversion);
	auto const crowdMetadata = core::agentPropertyMetadata(
		core::AgentPropertyType::CrowdAversion);
	auto const riskMetadata = core::agentPropertyMetadata(
		core::AgentPropertyType::RiskAversion);
	auto const familiarityMetadata = core::agentPropertyMetadata(
		core::AgentPropertyType::RouteFamiliarity);
	auto const persistenceMetadata = core::agentPropertyMetadata(
		core::AgentPropertyType::RoutePersistence);
	auto const ladderSpeedMetadata = core::agentPropertyMetadata(
		core::AgentPropertyType::LadderSpeedModifier);
	std::string ladderDiagnostic;
	require(ladderSpeedMetadata.name == "Ladder speed modifier"
		&& ladderSpeedMetadata.propertyNamespace == "Pathing"
		&& core::DefaultAgentLadderSpeedModifierRange == core::AgentModifierRange{}
		&& core::agentLadderSpeedModifierRangeIsValid({ 0.5f, 1.5f }, &ladderDiagnostic)
		&& !core::agentLadderSpeedModifierRangeIsValid({ 0.49f, 1.0f }, &ladderDiagnostic)
		&& ladderDiagnostic.find("between 0.5 and 1.5") != std::string::npos,
		"Ladder speed modifier metadata, default, or range contract is invalid");
	require(interactionMetadata.name == "Interaction aversion"
		&& interactionMetadata.propertyNamespace == "Pathing"
		&& effortMetadata.name == "Effort aversion"
		&& effortMetadata.propertyNamespace == "Pathing"
		&& waitingMetadata.name == "Waiting aversion"
		&& waitingMetadata.propertyNamespace == "Pathing"
		&& crowdMetadata.name == "Crowd aversion"
		&& crowdMetadata.propertyNamespace == "Pathing"
		&& riskMetadata.name == "Risk aversion"
		&& riskMetadata.propertyNamespace == "Pathing"
		&& familiarityMetadata.name == "Route familiarity"
		&& familiarityMetadata.propertyNamespace == "Pathing"
		&& persistenceMetadata.name == "Route persistence"
		&& persistenceMetadata.propertyNamespace == "Pathing"
		&& core::agentPropertyMetadata(core::AgentPropertyType::StairSpeedModifier).propertyNamespace
			== "Pathing"
		&& core::agentPropertyMetadata(core::AgentPropertyType::MobilityProfile).propertyNamespace
			== "Pathing"
		&& !core::agentPropertyMetadata(core::AgentPropertyType::Colour).propertyNamespace
		&& core::agentPropertyMetadata(core::AgentPropertyType::EscalatorWalkingChance).propertyNamespace
			== "Pathing"
		&& !core::agentPropertyMetadata(core::AgentPropertyType::WalkSpeedModifier).propertyNamespace
		&& !core::agentPropertyMetadata(core::AgentPropertyType::HeightModifier).propertyNamespace,
		"Agent property names and namespaces are not independently classified");

	auto registry = core::AgentTagRegistry::create();
	auto const tag = registry->addAgentTag("tag-values");
	std::string diagnostic;
	require(registry->addAgentTagColour(tag, &diagnostic), diagnostic);
	require(registry->setAgentTagColour(tag, { 10, 20, 30 }, &diagnostic), diagnostic);
	require(registry->addAgentTagEscalatorWalkingChance(tag, &diagnostic), diagnostic);
	require(registry->setAgentTagEscalatorWalkingChance(tag, 0.25f, &diagnostic), diagnostic);
	require(registry->addAgentTagWalkSpeedModifier(tag, &diagnostic), diagnostic);
	require(registry->setAgentTagWalkSpeedModifier(tag, { 0.9f, 0.9f }, &diagnostic), diagnostic);
	require(registry->addAgentTagHeightModifier(tag, &diagnostic), diagnostic);
	require(registry->setAgentTagHeightModifier(tag, { 0.8f, 0.8f }, &diagnostic), diagnostic);
	require(registry->addAgentTagStairSpeedModifier(tag, &diagnostic), diagnostic);
	require(registry->getAgentTagStairSpeedModifier(tag)->range
		== core::DefaultAgentStairSpeedModifierRange,
		"Stair speed modifier did not default to neutral");
	require(!registry->setAgentTagStairSpeedModifier(tag, { 0.49f, 1.0f }, &diagnostic)
		&& diagnostic.find("between 0.5 and 1.5") != std::string::npos,
		"Agent tag accepted an out-of-range Stair speed modifier");
	require(registry->setAgentTagStairSpeedModifier(tag, { 0.75f, 0.75f }, &diagnostic), diagnostic);
	require(registry->addAgentTagLadderSpeedModifier(tag, &diagnostic), diagnostic);
	require(registry->getAgentTagLadderSpeedModifier(tag)->range
		== core::DefaultAgentLadderSpeedModifierRange,
		"Ladder speed modifier did not default to neutral");
	require(registry->setAgentTagLadderSpeedModifier(tag, { 0.8f, 0.8f }, &diagnostic), diagnostic);
	require(registry->addAgentTagInteractionAversion(tag, &diagnostic), diagnostic);
	require(registry->setAgentTagInteractionAversion(tag, { 2.0f, 2.0f }, &diagnostic), diagnostic);
	require(registry->addAgentTagEffortAversion(tag, &diagnostic), diagnostic);
	require(registry->setAgentTagEffortAversion(tag, { 2.0f, 2.0f }, &diagnostic), diagnostic);
	require(registry->addAgentTagWaitingAversion(tag, &diagnostic), diagnostic);
	require(registry->getAgentTagWaitingAversion(tag)->range
		== core::DefaultAgentWaitingAversionRange,
		"Waiting aversion did not default to neutral");
	require(!registry->setAgentTagWaitingAversion(tag, { 0.49f, 1.0f }, &diagnostic)
		&& diagnostic.find("between 0.5 and 3") != std::string::npos,
		"Agent tag accepted an out-of-range Waiting aversion");
	require(registry->setAgentTagWaitingAversion(tag, { 2.5f, 2.5f }, &diagnostic), diagnostic);
	auto const registryYaml = serialize(*registry);
	require(registryYaml.find("version: 14") != std::string::npos
		&& registryYaml.find("type: ladderSpeedModifier") != std::string::npos
		&& registryYaml.find("type: waitingAversion") != std::string::npos
		&& registryYaml.find("min: 2.5") != std::string::npos,
		"The Agent tag Waiting aversion range was not persisted");
	require(registry->addAgentTagCrowdAversion(tag, &diagnostic), diagnostic);
	require(registry->getAgentTagCrowdAversion(tag)->range
		== core::DefaultAgentCrowdAversionRange,
		"Crowd aversion did not default to neutral");
	require(!registry->setAgentTagCrowdAversion(tag, { -0.01f, 1.0f }, &diagnostic)
		&& diagnostic.find("between 0 and 3") != std::string::npos,
		"Agent tag accepted an out-of-range Crowd aversion");
	require(registry->setAgentTagCrowdAversion(tag, { 2.5f, 2.5f }, &diagnostic), diagnostic);
	auto const crowdRegistryYaml = serialize(*registry);
	require(crowdRegistryYaml.find("version: 14") != std::string::npos
		&& crowdRegistryYaml.find("type: crowdAversion") != std::string::npos
		&& crowdRegistryYaml.find("min: 2.5") != std::string::npos,
		"The Agent tag Crowd aversion range was not persisted");
	require(registry->addAgentTagRiskAversion(tag, &diagnostic), diagnostic);
	require(registry->getAgentTagRiskAversion(tag)->range
		== core::DefaultAgentRiskAversionRange,
		"Risk aversion did not default to neutral");
	require(!registry->setAgentTagRiskAversion(tag, { -0.01f, 1.0f }, &diagnostic)
		&& diagnostic.find("between 0 and 3") != std::string::npos,
		"Agent tag accepted an out-of-range Risk aversion");
	require(registry->setAgentTagRiskAversion(tag, { 2.5f, 2.5f }, &diagnostic), diagnostic);
	auto const riskRegistryYaml = serialize(*registry);
	require(riskRegistryYaml.find("version: 14") != std::string::npos
		&& riskRegistryYaml.find("type: riskAversion") != std::string::npos,
		"The Agent tag Risk aversion range was not persisted");
	require(registry->addAgentTagRouteFamiliarity(tag, &diagnostic), diagnostic);
	require(registry->getAgentTagRouteFamiliarity(tag)->range
		== core::DefaultAgentRouteFamiliarityRange,
		"Route familiarity did not default to neutral");
	require(!registry->setAgentTagRouteFamiliarity(tag, { -0.01f, 1.0f }, &diagnostic)
		&& diagnostic.find("between 0 and 1") != std::string::npos,
		"Agent tag accepted an out-of-range Route familiarity");
	require(registry->setAgentTagRouteFamiliarity(tag, { 0.75f, 0.75f }, &diagnostic), diagnostic);
	auto const familiarityRegistryYaml = serialize(*registry);
	require(familiarityRegistryYaml.find("version: 14") != std::string::npos
		&& familiarityRegistryYaml.find("type: routeFamiliarity") != std::string::npos,
		"The Agent tag Route familiarity range was not persisted");
	require(registry->addAgentTagRoutePersistence(tag, &diagnostic), diagnostic);
	require(registry->getAgentTagRoutePersistence(tag)->range
		== core::DefaultAgentRoutePersistenceRange
		&& core::DefaultAgentRoutePersistenceRange.minimum == 0.15f,
		"Route persistence did not default to 0.15");
	require(!registry->setAgentTagRoutePersistence(tag, { -0.01f, 1.0f }, &diagnostic)
		&& diagnostic.find("between 0 and 1") != std::string::npos,
		"Agent tag accepted an out-of-range Route persistence");
	require(registry->setAgentTagRoutePersistence(tag, { 0.8f, 0.8f }, &diagnostic), diagnostic);
	require(serialize(*registry).find("type: routePersistence") != std::string::npos,
		"The Agent tag Route persistence range was not persisted");
	require(registry->addAgentTagMobilityProfile(tag, &diagnostic), diagnostic);
	core::MobilityProfile tagMobility;
	tagMobility.set(core::TraversalKind::Staircase, core::MobilityUse::CannotUse);
	require(registry->setAgentTagMobilityProfile(tag, tagMobility, &diagnostic), diagnostic);

	auto world = std::make_shared<core::World>("Individual properties", 8, 2);
	world->attachAgentTagRegistry("individual.tags.yaml", registry);
	auto const corridor = world->addCorridor(0, 0, 7);
	world->finishBuild();
	auto const id = world->createAgent("Agent", corridor, 0, 1.5f);
	world->pauseSimulation();
	require(world->assignAgentTag(id, tag, &diagnostic), diagnostic);
	auto* agent = world->lookupAgent(id).entity;
	require(agent->getEffectiveColour().value == (core::AgentColour{ 10, 20, 30 })
		&& !agent->getEffectiveColour().individual,
		"The fixture did not inherit its tag Colour");

	require(world->setAgentIndividualColour(id, core::AgentColour{ 100, 110, 120 }, &diagnostic), diagnostic);
	require(world->setAgentIndividualEscalatorWalkingChance(id, 0.75f, &diagnostic), diagnostic);
	require(world->setAgentIndividualWalkSpeedModifier(id, 1.1f, &diagnostic), diagnostic);
	require(world->setAgentIndividualHeightModifier(id, 0.95f, &diagnostic), diagnostic);
	require(agent->getStairSpeedModifierSample()
		&& agent->getStairSpeedModifierSample()->type == core::SampledAgentPropertyType::StairSpeedModifier
		&& agent->getEffectiveStairSpeedModifier().value == 0.75f
		&& agent->getEffectiveStairSpeedModifier().sourceTag == tag,
		"The Agent did not inherit its sampled Stair speed modifier");
	require(agent->getLadderSpeedModifierSample()
		&& agent->getLadderSpeedModifierSample()->type == core::SampledAgentPropertyType::LadderSpeedModifier
		&& agent->getEffectiveLadderSpeedModifier().value == 0.8f
		&& agent->getEffectiveLadderSpeedModifier().sourceTag == tag,
		"The Agent did not inherit its sampled Ladder speed modifier");
	require(agent->getEffectiveInteractionAversion().value == 2.0f
		&& agent->getEffectiveInteractionAversion().sourceTag == tag,
		"The Agent did not inherit its sampled Interaction aversion");
	require(world->setAgentIndividualStairSpeedModifier(id, 1.5f, &diagnostic), diagnostic);
	require(world->setAgentIndividualLadderSpeedModifier(id, 1.5f, &diagnostic), diagnostic);
	require(world->setAgentIndividualInteractionAversion(id, 0.0f, &diagnostic), diagnostic);
	require(agent->getEffectiveEffortAversion().value == 2.0f
		&& agent->getEffectiveEffortAversion().sourceTag == tag,
		"The Agent did not inherit its sampled Effort aversion");
	require(world->setAgentIndividualEffortAversion(id, 0.0f, &diagnostic), diagnostic);
	require(agent->getWaitingAversionSample()
		&& agent->getEffectiveWaitingAversion().value == 2.5f
		&& agent->getEffectiveWaitingAversion().sourceTag == tag,
		"The Agent did not inherit its sampled Waiting aversion");
	require(world->setAgentIndividualWaitingAversion(id, 0.5f, &diagnostic), diagnostic);
	require(agent->getCrowdAversionSample()
		&& agent->getEffectiveCrowdAversion().value == 2.5f
		&& agent->getEffectiveCrowdAversion().sourceTag == tag,
		"The Agent did not inherit its sampled Crowd aversion");
	require(world->setAgentIndividualCrowdAversion(id, 0.5f, &diagnostic), diagnostic);
	require(agent->getRiskAversionSample()
		&& agent->getEffectiveRiskAversion().value == 2.5f
		&& agent->getEffectiveRiskAversion().sourceTag == tag,
		"The Agent did not inherit its sampled Risk aversion");
	require(world->setAgentIndividualRiskAversion(id, 0.5f, &diagnostic), diagnostic);
	require(agent->getRouteFamiliaritySample()
		&& agent->getEffectiveRouteFamiliarity().value == 0.75f
		&& agent->getEffectiveRouteFamiliarity().sourceTag == tag,
		"The Agent did not inherit its sampled Route familiarity");
	require(world->setAgentIndividualRouteFamiliarity(id, 0.5f, &diagnostic), diagnostic);
	require(agent->getRoutePersistenceSample()
		&& agent->getEffectiveRoutePersistence().value == 0.8f
		&& agent->getEffectiveRoutePersistence().sourceTag == tag,
		"The Agent did not inherit its sampled Route persistence");
	require(world->setAgentIndividualRoutePersistence(id, 0.25f, &diagnostic), diagnostic);
	core::MobilityProfile directMobility;
	directMobility.set(core::TraversalKind::Lift, core::MobilityUse::CannotUse);
	require(world->setAgentIndividualMobilityProfile(id, directMobility, &diagnostic), diagnostic);

	require(agent->getEffectiveColour().individual
		&& agent->getEffectiveColour().value == (core::AgentColour{ 100, 110, 120 })
		&& agent->getEffectiveEscalatorWalkingChance().individual
		&& agent->getEffectiveEscalatorWalkingChance().value == 0.75f
		&& agent->getEffectiveWalkSpeedModifier().individual
		&& agent->getEffectiveWalkSpeedModifier().value == 1.1f
		&& agent->getEffectiveHeightModifier().individual
		&& agent->getEffectiveHeightModifier().value == 0.95f
		&& agent->getEffectiveStairSpeedModifier().individual
		&& agent->getEffectiveStairSpeedModifier().value == 1.5f
		&& agent->getEffectiveLadderSpeedModifier().individual
		&& agent->getEffectiveLadderSpeedModifier().value == 1.5f
		&& agent->getEffectiveInteractionAversion().individual
		&& agent->getEffectiveInteractionAversion().value == 0.0f
		&& agent->getEffectiveEffortAversion().individual
		&& agent->getEffectiveEffortAversion().value == 0.0f
		&& agent->getEffectiveWaitingAversion().individual
		&& agent->getEffectiveWaitingAversion().value == 0.5f
		&& agent->getEffectiveCrowdAversion().individual
		&& agent->getEffectiveCrowdAversion().value == 0.5f
		&& agent->getEffectiveRiskAversion().individual
		&& agent->getEffectiveRiskAversion().value == 0.5f
		&& agent->getEffectiveRouteFamiliarity().individual
		&& agent->getEffectiveRouteFamiliarity().value == 0.5f
		&& agent->getEffectiveRoutePersistence().individual
		&& agent->getEffectiveRoutePersistence().value == 0.25f
		&& agent->getEffectiveMobilityProfile().individual
		&& agent->getEffectiveMobilityProfile().value == directMobility,
		"Individual Agent properties did not override inherited tag values");
	require(core::agentForbidsTraversal(agent, core::TraversalKind::Lift)
		&& !core::agentForbidsTraversal(agent, core::TraversalKind::Staircase),
		"Routing did not use the individual Mobility profile instead of its tag");
	require(!world->setAgentIndividualStairSpeedModifier(id, 1.51f, &diagnostic)
		&& diagnostic.find("between 0.5 and 1.5") != std::string::npos,
		"An out-of-range individual Stair speed modifier was accepted");
	require(!world->setAgentIndividualLadderSpeedModifier(id, 1.51f, &diagnostic)
		&& diagnostic.find("between 0.5 and 1.5") != std::string::npos,
		"An out-of-range individual Ladder speed modifier was accepted");
	require(!world->setAgentIndividualWaitingAversion(id, 0.49f, &diagnostic)
		&& diagnostic.find("between 0.5 and 3") != std::string::npos,
		"An out-of-range individual Waiting aversion was accepted");
	require(!world->setAgentIndividualCrowdAversion(id, -0.01f, &diagnostic)
		&& diagnostic.find("between 0 and 3") != std::string::npos,
		"An out-of-range individual Crowd aversion was accepted");
	require(!world->setAgentIndividualRiskAversion(id, -0.01f, &diagnostic)
		&& diagnostic.find("between 0 and 3") != std::string::npos,
		"An out-of-range individual Risk aversion was accepted");
	require(!world->setAgentIndividualRouteFamiliarity(id, -0.01f, &diagnostic)
		&& diagnostic.find("between 0 and 1") != std::string::npos,
		"An out-of-range individual Route familiarity was accepted");
	require(!world->setAgentIndividualRoutePersistence(id, 1.01f, &diagnostic)
		&& diagnostic.find("between 0 and 1") != std::string::npos,
		"An out-of-range individual Route persistence was accepted");
	auto invalidMobility = directMobility;
	invalidMobility.uses[0] = static_cast<core::MobilityUse>(3);
	require(!world->setAgentIndividualMobilityProfile(id, invalidMobility, &diagnostic)
		&& diagnostic.find("invalid Mobility use") != std::string::npos,
		"An invalid individual Mobility use was accepted");
	auto const yaml = serialize(*world);
	require(yaml.find("version: 43") != std::string::npos
		&& yaml.find("individualProperties") != std::string::npos
		&& yaml.find("stairSpeedModifier") != std::string::npos
		&& yaml.find("ladderSpeedModifier") != std::string::npos
		&& yaml.find("interactionAversion") != std::string::npos
		&& yaml.find("effortAversion") != std::string::npos
		&& yaml.find("waitingAversion") != std::string::npos
		&& yaml.find("crowdAversion") != std::string::npos
		&& yaml.find("riskAversion") != std::string::npos
		&& yaml.find("routeFamiliarity") != std::string::npos
		&& yaml.find("routePersistence") != std::string::npos,
		"Individual Agent properties were not persisted in World schema 22");
	auto loaded = std::make_shared<core::World>("Loading", 1, 1);
	auto reader = core::YamlSerializer::fromString(yaml);
	reader->deserialize();
	core::SerializationWorkData work;
	require(loaded->deserialize(*reader, work), "The individual-property World did not deserialize");
	auto* loadedAgent = loaded->lookupAgent(id).entity;
	require(loadedAgent && loadedAgent->getStairSpeedModifierSample()
		&& loadedAgent->getStairSpeedModifierSample()->value == 0.75f
		&& loadedAgent->getLadderSpeedModifierSample()
		&& loadedAgent->getLadderSpeedModifierSample()->value == 0.8f
		&& loadedAgent->getEffectiveColour().individual
		&& loadedAgent->getEffectiveStairSpeedModifier().individual
		&& loadedAgent->getEffectiveStairSpeedModifier().value == 1.5f
		&& loadedAgent->getEffectiveLadderSpeedModifier().individual
		&& loadedAgent->getEffectiveLadderSpeedModifier().value == 1.5f
		&& loadedAgent->getEffectiveInteractionAversion().individual
		&& loadedAgent->getEffectiveInteractionAversion().value == 0.0f
		&& loadedAgent->getEffectiveEffortAversion().individual
		&& loadedAgent->getEffectiveEffortAversion().value == 0.0f
		&& loadedAgent->getWaitingAversionSample()
		&& loadedAgent->getWaitingAversionSample()->value == 2.5f
		&& loadedAgent->getEffectiveWaitingAversion().individual
		&& loadedAgent->getEffectiveWaitingAversion().value == 0.5f
		&& loadedAgent->getCrowdAversionSample()
		&& loadedAgent->getCrowdAversionSample()->value == 2.5f
		&& loadedAgent->getEffectiveCrowdAversion().individual
		&& loadedAgent->getEffectiveCrowdAversion().value == 0.5f
		&& loadedAgent->getRiskAversionSample()
		&& loadedAgent->getRiskAversionSample()->value == 2.5f
		&& loadedAgent->getEffectiveRiskAversion().individual
		&& loadedAgent->getEffectiveRiskAversion().value == 0.5f
		&& loadedAgent->getRouteFamiliaritySample()
		&& loadedAgent->getRouteFamiliaritySample()->value == 0.75f
		&& loadedAgent->getRoutePersistenceSample()
		&& loadedAgent->getRoutePersistenceSample()->value == 0.8f
		&& loadedAgent->getEffectiveRoutePersistence().individual
		&& loadedAgent->getEffectiveRoutePersistence().value == 0.25f
		&& loadedAgent->getEffectiveRouteFamiliarity().individual
		&& loadedAgent->getEffectiveRouteFamiliarity().value == 0.5f
		&& loadedAgent->getEffectiveMobilityProfile().value == directMobility,
		"Individual Agent properties did not round-trip");

	require(world->setAgentIndividualColour(id, std::nullopt, &diagnostic), diagnostic);
	require(!agent->getEffectiveColour().individual
		&& agent->getEffectiveColour().sourceTag == tag
		&& agent->getEffectiveColour().value == (core::AgentColour{ 10, 20, 30 }),
		"Removing an individual property did not reveal its inherited tag value");

	require(world->resumeSimulation(), "The individual-property fixture did not resume");
	require(!world->setAgentIndividualHeightModifier(id, 0.9f, &diagnostic)
		&& diagnostic.find("Pause") != std::string::npos,
		"An individual Agent property changed while simulation was running");
}

void agent_smoke::registerIndividualProperties(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "individualPropertiesOverrideTagPropertiesAndPersist", [](smoke::Context const&) { individualPropertiesOverrideTagPropertiesAndPersist(); } });
}
