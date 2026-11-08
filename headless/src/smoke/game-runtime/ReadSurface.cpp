#include "Checks.h"
#include "Fixture.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "core/World.h"
#include "game/WorldRuntime.h"

// The full read surface (#966, spec #960): every read `core::World` answers
// through a `const` member is answered by `game::WorldRuntime`, and answered
// through the `const` overload rather than by widening the World back into a
// mutable handle.
//
// Two halves prove that, and they fail in different ways on purpose:
//
//   * the compile-time block below pins every read to a `WorldRuntime const&`
//     and to the exact type the World's const overload returns. A read that
//     is missing, that is not `const`, or that resolves the mutable overload
//     and so reports a mutable entity, stops the build. That is the whole
//     surface, not a sample: the list is one entry per forwarded read.
//   * the runtime checks then show the pinned surface actually answers live.
//     Each read is taken twice - once through the runtime, once through the
//     `core::World const&` the runtime hands to rendering - and the two must
//     agree, which is what makes "the runtime forwards" a tested claim rather
//     than a comment. A sweep is also fingerprinted before and after, so a
//     read that disturbed observable state cannot pass unnoticed.

// `call` must resolve on a `WorldRuntime const&` and return exactly `RetType`.
// Written against a named parameter rather than a spelled-out object so that a
// missing member, a dropped `const` or a wrong overload is a substitution
// failure the requires-expression can report.
#define PF_REQUIRE_CONST_READ(call, RetType) \
	static_assert(requires(::game::WorldRuntime const& surface) { \
		{ surface.call } -> ::std::same_as<RetType>; \
	}, "read is not reachable through a const WorldRuntime with the expected type: " #call);

// --- Every forwarded read, pinned to the const view and its return type ---
PF_REQUIRE_CONST_READ(getName(), std::string const&)
PF_REQUIRE_CONST_READ(getClipboardIdentity(), std::string const&)
PF_REQUIRE_CONST_READ(getLifetimeToken(), std::weak_ptr<void const>)
PF_REQUIRE_CONST_READ(getRandomSeed(), std::uint64_t)
PF_REQUIRE_CONST_READ(documentDirectory(), std::filesystem::path const&)
PF_REQUIRE_CONST_READ(getLoadWarnings(), std::vector<std::string> const&)
PF_REQUIRE_CONST_READ(getBuildLog(), core::Log const&)
PF_REQUIRE_CONST_READ(getCellsWide(), std::uint32_t)
PF_REQUIRE_CONST_READ(getLevelsHigh(), std::uint32_t)
PF_REQUIRE_CONST_READ(getLayerCount(), std::uint32_t)
PF_REQUIRE_CONST_READ(getLayerName(::std::declval<std::uint32_t>()), std::string const&)
PF_REQUIRE_CONST_READ(getLevelName(::std::declval<std::uint32_t>()), std::string const&)
PF_REQUIRE_CONST_READ(getNumSectors(), std::uint32_t)
PF_REQUIRE_CONST_READ(getLayer(::std::declval<std::uint32_t>()), std::shared_ptr<const core::Layer>)
PF_REQUIRE_CONST_READ(getGraph(), std::shared_ptr<const core::Graph>)
PF_REQUIRE_CONST_READ(getSector(::std::declval<std::uint32_t>()), std::shared_ptr<const core::Sector>)
PF_REQUIRE_CONST_READ(getSectorsInBounds(::std::declval<std::uint32_t>(), ::std::declval<float>(), ::std::declval<float>(), ::std::declval<float>(), ::std::declval<float>()), std::vector<std::shared_ptr<const core::Sector>>)
PF_REQUIRE_CONST_READ(getSectors(::std::declval<std::uint32_t>()), std::vector<std::shared_ptr<const core::Sector>>)
PF_REQUIRE_CONST_READ(getSectorAtPosition(::std::declval<std::uint32_t>(), ::std::declval<float>(), ::std::declval<float>()), std::shared_ptr<const core::Sector>)
PF_REQUIRE_CONST_READ(getAgentAtPosition(::std::declval<std::uint32_t>(), ::std::declval<float>(), ::std::declval<float>()), core::Agent*)
PF_REQUIRE_CONST_READ(getObjectAtPosition(::std::declval<std::uint32_t>(), ::std::declval<float>(), ::std::declval<float>(), ::std::declval<std::shared_ptr<const core::SectorObject>*>()), std::shared_ptr<const core::Object>)
PF_REQUIRE_CONST_READ(getPausedPathIntent(::std::declval<core::Agent const&>(), ::std::declval<core::World::TopologyPathIntent&>()), bool)
PF_REQUIRE_CONST_READ(isTraversalTopologyDirty(), bool)
PF_REQUIRE_CONST_READ(isTraversalTopologyValid(), bool)
PF_REQUIRE_CONST_READ(getTopologyGeneration(), std::uint64_t)
PF_REQUIRE_CONST_READ(getTopologyDiagnostic(), std::string const&)
PF_REQUIRE_CONST_READ(hasDumbwaiters(), bool)
PF_REQUIRE_CONST_READ(lookupDumbwaiter(::std::declval<core::DumbwaiterId>()), std::shared_ptr<const core::Dumbwaiter>)
PF_REQUIRE_CONST_READ(isDumbwaiterOwnedControl(::std::declval<std::shared_ptr<const core::SectorObject> const&>()), bool)
PF_REQUIRE_CONST_READ(isAirlockOwnedObject(::std::declval<std::shared_ptr<const core::SectorObject> const&>()), bool)
PF_REQUIRE_CONST_READ(isChamberOwnedObject(::std::declval<std::shared_ptr<const core::SectorObject> const&>()), bool)
PF_REQUIRE_CONST_READ(isLiftOwnedDoor(::std::declval<std::shared_ptr<const core::SectorObject> const&>(), ::std::declval<std::uint32_t*>(), ::std::declval<std::uint32_t*>()), bool)
PF_REQUIRE_CONST_READ(isLiftOwnedControl(::std::declval<std::shared_ptr<const core::SectorObject> const&>(), ::std::declval<std::uint32_t*>(), ::std::declval<std::uint32_t*>()), bool)
PF_REQUIRE_CONST_READ(isShuttleOwnedDoor(::std::declval<std::shared_ptr<const core::SectorObject> const&>(), ::std::declval<std::uint32_t*>(), ::std::declval<std::uint32_t*>(), ::std::declval<std::uint32_t*>(), ::std::declval<std::uint32_t*>()), bool)
PF_REQUIRE_CONST_READ(isShuttleOwnedControl(::std::declval<std::shared_ptr<const core::SectorObject> const&>(), ::std::declval<std::uint32_t*>(), ::std::declval<std::uint32_t*>()), bool)
PF_REQUIRE_CONST_READ(isBulkheadDoorOwnedControl(::std::declval<std::shared_ptr<const core::SectorObject> const&>(), ::std::declval<std::uint32_t*>(), ::std::declval<std::uint32_t*>()), bool)
PF_REQUIRE_CONST_READ(isForceBridgeOwnedControl(::std::declval<std::shared_ptr<const core::SectorObject> const&>(), ::std::declval<std::uint32_t*>(), ::std::declval<std::uint32_t*>()), bool)
PF_REQUIRE_CONST_READ(isControlTargetBroken(::std::declval<core::InteractionPointId>()), bool)
PF_REQUIRE_CONST_READ(getLadderOptions(::std::declval<std::uint32_t>(), ::std::declval<core::World::CreateLadderOptions&>()), bool)
PF_REQUIRE_CONST_READ(getStairwellOptions(::std::declval<std::uint32_t>(), ::std::declval<core::World::CreateStairwellOptions&>()), bool)
PF_REQUIRE_CONST_READ(getStaircaseOptions(::std::declval<std::uint32_t>(), ::std::declval<core::World::CreateStaircaseOptions&>()), bool)
PF_REQUIRE_CONST_READ(getLiftLandingRows(::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>()), std::vector<core::World::LiftLandingRow>)
PF_REQUIRE_CONST_READ(getLiftLandingGeometry(::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t&>(), ::std::declval<std::uint32_t&>()), bool)
PF_REQUIRE_CONST_READ(getValidShuttleStopOffsets(::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>(), ::std::declval<bool>(), ::std::declval<std::uint32_t>()), std::vector<std::uint32_t>)
PF_REQUIRE_CONST_READ(getShuttleOptions(::std::declval<core::Shuttle const*>(), ::std::declval<core::World::CreateShuttleOptions&>()), bool)
PF_REQUIRE_CONST_READ(getShuttleStopCandidatesForDoor(::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>()), std::vector<core::World::ShuttleStopCandidate>)
PF_REQUIRE_CONST_READ(getSectorDoorOptions(::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>(), ::std::declval<core::World::CreateDoorOptions&>()), bool)
PF_REQUIRE_CONST_READ(getSectorWindowOptions(::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>(), ::std::declval<core::World::CreateWindowOptions&>()), bool)
PF_REQUIRE_CONST_READ(getSectorBulkheadDoorOptions(::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>(), ::std::declval<core::World::CreateBulkheadDoorOptions&>()), bool)
PF_REQUIRE_CONST_READ(calculateSectorForceBridgeWidthToRight(::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t&>(), ::std::declval<std::string*>()), bool)
PF_REQUIRE_CONST_READ(getSectorForceBridgeOptions(::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>(), ::std::declval<core::World::CreateForceBridgeOptions&>()), bool)
PF_REQUIRE_CONST_READ(getRoomLadderOptions(::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>(), ::std::declval<core::World::CreateLadderOptions&>()), bool)
PF_REQUIRE_CONST_READ(getPlatformLiftStopCandidates(::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>()), std::vector<core::World::PlatformLiftStopCandidate>)
PF_REQUIRE_CONST_READ(getPlatformLiftOptions(::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>(), ::std::declval<core::World::CreateLiftOptions&>()), bool)
PF_REQUIRE_CONST_READ(availableAgentActions(::std::declval<core::MarkerId>()), std::vector<std::string>)
PF_REQUIRE_CONST_READ(markerActions(::std::declval<core::MarkerId>()), std::vector<std::string>)
PF_REQUIRE_CONST_READ(agentActionDisplayName(::std::declval<std::string_view>()), std::string)
PF_REQUIRE_CONST_READ(getMarkerIds(), std::vector<core::MarkerId>)
PF_REQUIRE_CONST_READ(lookupMarker(::std::declval<core::MarkerId>()), std::shared_ptr<const core::Marker>)
PF_REQUIRE_CONST_READ(furnitureUseEligible(::std::declval<core::AgentId>(), ::std::declval<core::MarkerId>(), ::std::declval<std::string*>()), bool)
PF_REQUIRE_CONST_READ(agentPoseStateFits(::std::declval<core::Agent const&>(), ::std::declval<float>(), ::std::declval<float>()), bool)
PF_REQUIRE_CONST_READ(agentHeightStateFits(::std::declval<core::Agent const&>(), ::std::declval<float>(), ::std::declval<std::string*>()), bool)
PF_REQUIRE_CONST_READ(validateReplayPoseState(::std::declval<core::World const&>(), ::std::declval<std::uint32_t>(), ::std::declval<int>(), ::std::declval<int>()), void)
PF_REQUIRE_CONST_READ(furniturePoseFits(::std::declval<core::AgentId>(), ::std::declval<core::MarkerId>(), ::std::declval<core::Pose>(), ::std::declval<bool>(), ::std::declval<std::string*>()), bool)
PF_REQUIRE_CONST_READ(maximumInstalledStandingWidth(), float)
PF_REQUIRE_CONST_READ(hasAgentType(::std::declval<std::string_view>()), bool)
PF_REQUIRE_CONST_READ(agentTypeDisplayName(::std::declval<std::string_view>()), std::string)
PF_REQUIRE_CONST_READ(agentTypeResourceName(::std::declval<std::string_view>()), std::string)
PF_REQUIRE_CONST_READ(registeredAgentTypeDefinition(::std::declval<std::string_view>(), ::std::declval<std::string_view>()), std::optional<core::AgentTypeDefinition>)
PF_REQUIRE_CONST_READ(registeredAgentTypeBaseline(::std::declval<std::string_view>(), ::std::declval<std::string_view>()), std::optional<core::AgentPhysicalBaseline>)
PF_REQUIRE_CONST_READ(validateAgentLocationPlacement(::std::declval<core::Sector const&>(), ::std::declval<core::Agent const&>(), ::std::declval<std::optional<float>>()), void)
PF_REQUIRE_CONST_READ(lookupAgent(::std::declval<core::AgentId>()), core::EntityLookup<core::Agent const>)
PF_REQUIRE_CONST_READ(canSetAgentActive(::std::declval<core::AgentId>(), ::std::declval<bool>(), ::std::declval<std::string*>()), bool)
PF_REQUIRE_CONST_READ(getAgentId(::std::declval<core::Agent const*>()), core::AgentId)
PF_REQUIRE_CONST_READ(hasAgentBehaviourRegistryReference(), bool)
PF_REQUIRE_CONST_READ(hasAttachedAgentBehaviourRegistry(), bool)
PF_REQUIRE_CONST_READ(getExpectedAgentBehaviourRegistryUuid(), std::string const&)
PF_REQUIRE_CONST_READ(getAgentBehaviourRegistry(), std::shared_ptr<core::AgentBehaviourRegistry> const&)
PF_REQUIRE_CONST_READ(agentBehaviourConfigurationsAreValid(), bool)
PF_REQUIRE_CONST_READ(getAgentBehaviourDependencyDiagnostic(), std::string const&)
PF_REQUIRE_CONST_READ(validateAgentBehaviourAssignment(::std::declval<core::AgentBehaviourId>(), ::std::declval<std::uint64_t>(), ::std::declval<core::AgentBehaviourConfiguration const&>(), ::std::declval<core::AgentBehaviourConfiguration*>(), ::std::declval<std::string*>()), bool)
PF_REQUIRE_CONST_READ(getAgentBehaviourAssignment(::std::declval<core::AgentId>()), std::optional<core::AgentBehaviourAssignment> const&)
PF_REQUIRE_CONST_READ(getAgentBehaviourAssignmentCount(), std::uint32_t)
PF_REQUIRE_CONST_READ(agentBehaviourOwnsMovement(::std::declval<core::AgentId>()), bool)
PF_REQUIRE_CONST_READ(getAgentBehaviourRuntimeDiagnostics(), std::vector<core::AgentBehaviourRuntimeDiagnostic>)
PF_REQUIRE_CONST_READ(getAgentBehaviourRuntimeLimits(), core::AgentBehaviourRuntimeLimits)
PF_REQUIRE_CONST_READ(actionRegistry(), std::shared_ptr<const core::ActionRegistry> const&)
PF_REQUIRE_CONST_READ(hasAgentTagRegistryReference(), bool)
PF_REQUIRE_CONST_READ(hasAttachedAgentTagRegistry(), bool)
PF_REQUIRE_CONST_READ(getExpectedAgentTagRegistryUuid(), std::string const&)
PF_REQUIRE_CONST_READ(getAgentTagRegistry(), std::shared_ptr<core::AgentTagRegistry> const&)
PF_REQUIRE_CONST_READ(getAgentTagAssignmentCount(), std::uint64_t)
PF_REQUIRE_CONST_READ(getAgentTagAssignedAgentCount(), std::uint32_t)
PF_REQUIRE_CONST_READ(getAgentTagSampleCount(), std::uint64_t)
PF_REQUIRE_CONST_READ(canAssignAgentTag(::std::declval<core::AgentId>(), ::std::declval<core::AgentTagId>(), ::std::declval<std::string*>()), bool)
PF_REQUIRE_CONST_READ(validateAgentTagAssignments(::std::declval<std::set<core::AgentTagId> const&>(), ::std::declval<std::optional<core::AgentPropertySample> const&>(), ::std::declval<std::optional<core::AgentPropertySample> const&>(), ::std::declval<std::optional<core::AgentPropertySample> const&>(), ::std::declval<std::optional<core::AgentPropertySample> const&>(), ::std::declval<std::optional<core::AgentPropertySample> const&>(), ::std::declval<std::optional<core::AgentPropertySample> const&>(), ::std::declval<std::optional<core::AgentPropertySample> const&>(), ::std::declval<std::optional<core::AgentPropertySample> const&>(), ::std::declval<std::optional<core::AgentPropertySample> const&>(), ::std::declval<std::optional<core::AgentPropertySample> const&>(), ::std::declval<std::optional<core::AgentPropertySample> const&>(), ::std::declval<std::optional<core::AgentPropertySample> const&>(), ::std::declval<std::optional<core::AgentPropertySample> const&>(), ::std::declval<std::string*>()), bool)
PF_REQUIRE_CONST_READ(getAgentTags(::std::declval<core::AgentId>()), std::set<core::AgentTagId> const&)
PF_REQUIRE_CONST_READ(getAgentGroupCount(), std::uint32_t)
PF_REQUIRE_CONST_READ(getAgentGroupIds(), std::vector<core::AgentGroupId>)
PF_REQUIRE_CONST_READ(lookupAgentGroup(::std::declval<core::AgentGroupId>()), core::EntityLookup<core::AgentGroup const>)
PF_REQUIRE_CONST_READ(getAgentGroupName(::std::declval<core::AgentGroupId>()), std::string const&)
PF_REQUIRE_CONST_READ(canSetAgentGroup(::std::declval<core::AgentId>(), ::std::declval<core::AgentGroupId>(), ::std::declval<std::string*>()), bool)
PF_REQUIRE_CONST_READ(getAgentGroup(::std::declval<core::AgentId>()), core::AgentGroupId)
PF_REQUIRE_CONST_READ(isAgentGroupActive(::std::declval<core::AgentGroupId>()), bool)
PF_REQUIRE_CONST_READ(canSetAgentGroupActive(::std::declval<core::AgentGroupId>(), ::std::declval<bool>(), ::std::declval<std::string*>()), bool)
PF_REQUIRE_CONST_READ(getAgentGroupMemberCount(::std::declval<core::AgentGroupId>()), std::uint32_t)
PF_REQUIRE_CONST_READ(getAccessPermissionCount(), std::uint32_t)
PF_REQUIRE_CONST_READ(getAccessPermissionIds(), std::vector<core::AccessPermissionId>)
PF_REQUIRE_CONST_READ(lookupAccessPermission(::std::declval<core::AccessPermissionId>()), core::EntityLookup<core::AccessPermission const>)
PF_REQUIRE_CONST_READ(getAccessPermissionName(::std::declval<core::AccessPermissionId>()), std::string const&)
PF_REQUIRE_CONST_READ(getAccessPermissionUsage(::std::declval<core::AccessPermissionId>()), core::World::AccessPermissionUsage)
PF_REQUIRE_CONST_READ(getAgentDirectAccessGrants(::std::declval<core::AgentId>()), std::vector<core::AccessPermissionId>)
PF_REQUIRE_CONST_READ(getAgentCurrentDirectAccessGrants(::std::declval<core::AgentId>()), std::vector<core::AccessPermissionId>)
PF_REQUIRE_CONST_READ(getAgentCurrentPermissionSetAssignments(::std::declval<core::AgentId>()), std::vector<core::PermissionSetId>)
PF_REQUIRE_CONST_READ(getAgentEffectiveAccessGrants(::std::declval<core::AgentId>()), std::vector<core::AccessPermissionId>)
PF_REQUIRE_CONST_READ(getAgentAccessGrantSources(::std::declval<core::AgentId>(), ::std::declval<core::AccessPermissionId>()), core::World::EffectiveAccessGrantSources)
PF_REQUIRE_CONST_READ(getPermissionSetCount(), std::uint32_t)
PF_REQUIRE_CONST_READ(getPermissionSetIds(), std::vector<core::PermissionSetId>)
PF_REQUIRE_CONST_READ(lookupPermissionSet(::std::declval<core::PermissionSetId>()), core::EntityLookup<core::PermissionSet const>)
PF_REQUIRE_CONST_READ(getPermissionSetName(::std::declval<core::PermissionSetId>()), std::string const&)
PF_REQUIRE_CONST_READ(getPermissionSetUsageCount(::std::declval<core::PermissionSetId>()), std::uint32_t)
PF_REQUIRE_CONST_READ(getPermissionSetPermissions(::std::declval<core::PermissionSetId>()), std::vector<core::AccessPermissionId>)
PF_REQUIRE_CONST_READ(getAgentPermissionSetAssignments(::std::declval<core::AgentId>()), std::vector<core::PermissionSetId>)
PF_REQUIRE_CONST_READ(isLocationPermissionEligible(::std::declval<std::uint32_t>()), bool)
PF_REQUIRE_CONST_READ(getLocationPermissionRequirement(::std::declval<std::uint32_t>()), std::vector<core::AccessPermissionId>)
PF_REQUIRE_CONST_READ(canAgentAccessLocation(::std::declval<core::Sector const&>(), ::std::declval<core::Agent const&>()), bool)
PF_REQUIRE_CONST_READ(isInteractionPointPermissionEligible(::std::declval<core::InteractionPointId>()), bool)
PF_REQUIRE_CONST_READ(getInteractionPointPermissionRequirement(::std::declval<core::InteractionPointId>()), std::vector<core::AccessPermissionId>)
PF_REQUIRE_CONST_READ(isManualDoorPermissionEligible(::std::declval<core::TraversalResourceId>()), bool)
PF_REQUIRE_CONST_READ(getManualDoorPermissionRequirement(::std::declval<core::TraversalResourceId>()), std::vector<core::AccessPermissionId>)
PF_REQUIRE_CONST_READ(getTransportStopShortName(::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>()), std::string)
PF_REQUIRE_CONST_READ(getTransportStopShortName(::std::declval<core::TraversalResourceId>(), ::std::declval<std::uint32_t>()), std::string)
PF_REQUIRE_CONST_READ(getLiftDestinationLevels(::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>()), std::vector<std::uint32_t>)
PF_REQUIRE_CONST_READ(getLiftDestinationPermissionRequirement(::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>(), ::std::declval<std::uint32_t>()), std::vector<core::AccessPermissionId>)
PF_REQUIRE_CONST_READ(knownTransportCondition(::std::declval<core::TraversalResourceId>(), ::std::declval<core::Agent const*>(), ::std::declval<core::Sector const*>()), std::optional<core::DeviceCondition>)
PF_REQUIRE_CONST_READ(knownLiftCondition(::std::declval<core::TraversalResourceId>(), ::std::declval<core::Agent const*>(), ::std::declval<core::Sector const*>()), std::optional<core::DeviceCondition>)
PF_REQUIRE_CONST_READ(canAgentEnterAirlock(::std::declval<core::TraversalResourceId>(), ::std::declval<core::SectorId>(), ::std::declval<core::AgentId>(), ::std::declval<bool>()), bool)
PF_REQUIRE_CONST_READ(canAgentOpenManualDoor(::std::declval<core::TraversalResourceId>(), ::std::declval<core::AgentId>()), bool)
PF_REQUIRE_CONST_READ(agentHasFeasibleManualDoorOperation(::std::declval<core::TraversalResourceId>(), ::std::declval<core::AgentId>()), bool)
PF_REQUIRE_CONST_READ(remoteButtonApproachDistance(::std::declval<core::InteractionPointId>(), ::std::declval<core::SectorId>(), ::std::declval<core::Vector2 const&>(), ::std::declval<core::AgentId>()), std::optional<float>)
PF_REQUIRE_CONST_READ(canAgentOperateDoorControl(::std::declval<core::TraversalResourceId>(), ::std::declval<core::SectorId>(), ::std::declval<core::AgentId>()), bool)
PF_REQUIRE_CONST_READ(agentAdheresToDoorPermission(::std::declval<core::TraversalResourceId>(), ::std::declval<core::SectorId>(), ::std::declval<core::AgentId>()), bool)
PF_REQUIRE_CONST_READ(canAgentOperateExtensibleControl(::std::declval<core::TraversalResourceId>(), ::std::declval<core::SectorId>(), ::std::declval<core::Vector2 const&>(), ::std::declval<core::AgentId>()), bool)
PF_REQUIRE_CONST_READ(agentAdheresToExtensiblePermission(::std::declval<core::TraversalResourceId>(), ::std::declval<core::SectorId>(), ::std::declval<core::Vector2 const&>(), ::std::declval<core::AgentId>()), bool)
PF_REQUIRE_CONST_READ(missingLiftDestinationPermissions(::std::declval<core::DeviceCommand const&>(), ::std::declval<core::AgentId>()), std::vector<core::AccessPermissionId>)
PF_REQUIRE_CONST_READ(canAgentUseLiftJourney(::std::declval<core::TraversalResourceId>(), ::std::declval<core::Vector2 const&>(), ::std::declval<core::Vector2 const&>(), ::std::declval<core::AgentId>()), bool)
PF_REQUIRE_CONST_READ(agentHasFeasibleLiftJourney(::std::declval<core::TraversalResourceId>(), ::std::declval<core::Vector2 const&>(), ::std::declval<core::Vector2 const&>(), ::std::declval<core::AgentId>()), bool)
PF_REQUIRE_CONST_READ(agentAdheresToLiftDestinationPermission(::std::declval<core::TraversalResourceId>(), ::std::declval<std::uint32_t>(), ::std::declval<core::AgentId>()), bool)
PF_REQUIRE_CONST_READ(canAgentOperateTransportLandingControl(::std::declval<core::TraversalResourceId>(), ::std::declval<core::SectorId>(), ::std::declval<core::Vector2 const&>(), ::std::declval<core::AgentId>()), bool)
PF_REQUIRE_CONST_READ(agentHasFeasibleTransportLandingOperation(::std::declval<core::TraversalResourceId>(), ::std::declval<core::SectorId>(), ::std::declval<core::Vector2 const&>(), ::std::declval<core::AgentId>()), bool)
PF_REQUIRE_CONST_READ(agentAdheresToTransportLandingPermission(::std::declval<core::TraversalResourceId>(), ::std::declval<core::SectorId>(), ::std::declval<core::Vector2 const&>(), ::std::declval<core::AgentId>()), bool)
PF_REQUIRE_CONST_READ(isAgentTransportOccupant(::std::declval<core::TraversalResourceId>(), ::std::declval<core::AgentId>()), bool)
PF_REQUIRE_CONST_READ(isTransportLocallyBoardable(::std::declval<core::TraversalResourceId>(), ::std::declval<core::Vector2 const&>()), bool)
PF_REQUIRE_CONST_READ(canAgentTraverseManualDoorNow(::std::declval<core::TraversalResourceId>(), ::std::declval<core::AgentId>()), bool)
PF_REQUIRE_CONST_READ(lookupInteractionPoint(::std::declval<core::InteractionPointId>()), core::EntityLookup<core::InteractionPoint const>)
PF_REQUIRE_CONST_READ(lookupAccessPanel(::std::declval<core::AccessPanelId>()), std::shared_ptr<const core::AccessPanel>)
PF_REQUIRE_CONST_READ(canRequestAccessPanel(::std::declval<core::AccessPanelId>(), ::std::declval<core::AccessPanel::Action>(), ::std::declval<core::AgentId>()), bool)
PF_REQUIRE_CONST_READ(lookupInteractionRequest(::std::declval<core::InteractionRequestId>()), core::EntityLookup<core::InteractionRequest const>)
PF_REQUIRE_CONST_READ(lookupBoothWindow(::std::declval<core::BoothWindowId>()), std::shared_ptr<const core::BoothWindow>)
PF_REQUIRE_CONST_READ(lookupDeviceOperation(::std::declval<core::DeviceOperationId>()), core::EntityLookup<core::DeviceOperation const>)
PF_REQUIRE_CONST_READ(lookupTraversalResource(::std::declval<core::TraversalResourceId>()), core::EntityLookup<core::TraversalResource const>)
PF_REQUIRE_CONST_READ(getTraversalResourceId(::std::declval<core::Object const*>()), core::TraversalResourceId)
PF_REQUIRE_CONST_READ(lookupTraversalRequest(::std::declval<core::TraversalRequestId>()), core::EntityLookup<core::TraversalRequest const>)
PF_REQUIRE_CONST_READ(isAgentInQueue(::std::declval<core::AgentId>()), bool)
PF_REQUIRE_CONST_READ(getRouteChoicePolicy(), core::RouteChoicePolicy const&)
PF_REQUIRE_CONST_READ(getTraversalWaitingPolicy(), core::TraversalWaitingPolicy const&)
PF_REQUIRE_CONST_READ(getTraversalGeometryPolicy(), core::TraversalGeometryPolicy const&)
PF_REQUIRE_CONST_READ(estimateTraversalDelay(::std::declval<core::TraversalResourceId>(), ::std::declval<core::SectorId>()), float)
PF_REQUIRE_CONST_READ(observeAccessZoneDensity(::std::declval<core::TraversalResourceId>(), ::std::declval<core::SectorId>()), float)
PF_REQUIRE_CONST_READ(observeShuttleAccess(::std::declval<core::TraversalResourceId>(), ::std::declval<core::Vector2 const&>(), ::std::declval<bool>()), std::optional<core::ShuttleRouteAccessObservation>)
PF_REQUIRE_CONST_READ(observeLiftAccess(::std::declval<core::TraversalResourceId>(), ::std::declval<core::Vector2 const&>(), ::std::declval<bool>()), std::optional<core::LiftRouteAccessObservation>)
PF_REQUIRE_CONST_READ(countStandingAgentsOnEscalator(::std::declval<core::Agent const*>(), ::std::declval<core::Edge const*>()), std::uint32_t)
PF_REQUIRE_CONST_READ(furnitureCatalogue(), std::shared_ptr<const core::FurnitureCatalogue> const&)
PF_REQUIRE_CONST_READ(furniture(), std::vector<core::FurnitureInstance> const&)
PF_REQUIRE_CONST_READ(isFurnitureMarker(::std::declval<core::MarkerId>()), bool)
PF_REQUIRE_CONST_READ(usablePointOccupant(::std::declval<core::MarkerId>()), core::AgentId)
PF_REQUIRE_CONST_READ(getDeferredSimulationTime(), double)
PF_REQUIRE_CONST_READ(getAverageSimulationStepMicroseconds(), double)
PF_REQUIRE_CONST_READ(getSimulationEvents(), std::vector<core::SimulationEvent> const&)
#ifndef PF_CAT
#define PF_CAT_IMPL(a, b) a##b
#define PF_CAT(a, b) PF_CAT_IMPL(a, b)
#endif

// The other half of the read-surface contract (#966): nothing that writes is
// reachable here. The authoring module's Boundary.cpp (#968, #969) already
// proves the authoring, binding and persistence families absent; these pin
// the mutation family, which is the one a read surface could most easily
// drift into by forwarding a `set...` "because it has a getter".
#define PF_ASSERT_UNREACHABLE(Type, MemberCall, Message) \
	template <typename T, typename = void> \
	struct PF_CAT(PfReadSurfaceReachable, __LINE__) : std::false_type {}; \
	template <typename T> \
	struct PF_CAT(PfReadSurfaceReachable, __LINE__)<T, \
		::std::void_t<decltype(::std::declval<T>().MemberCall)>> : ::std::true_type {}; \
	static_assert(!PF_CAT(PfReadSurfaceReachable, __LINE__)<Type>::value, Message);

PF_ASSERT_UNREACHABLE(game::WorldRuntime&, setLayerName(0u, std::string{}),
	"a Layer rename is reachable from WorldRuntime (#966)")
PF_ASSERT_UNREACHABLE(game::WorldRuntime&, setLevelName(0u, std::string{}),
	"a Level rename is reachable from WorldRuntime (#966)")
PF_ASSERT_UNREACHABLE(game::WorldRuntime&, setAgentActive(core::AgentId{}, true),
	"an Agent activation is reachable from WorldRuntime (#966)")
PF_ASSERT_UNREACHABLE(game::WorldRuntime&,
	setAgentIndividualColour(core::AgentId{}, std::nullopt),
	"an Agent property write is reachable from WorldRuntime (#966)")
PF_ASSERT_UNREACHABLE(game::WorldRuntime&,
	grantAgentAccessPermission(core::AgentId{}, core::AccessPermissionId{}),
	"an access grant is reachable from WorldRuntime (#966)")
PF_ASSERT_UNREACHABLE(game::WorldRuntime&, setMarkerActions(core::MarkerId{}, {}),
	"a Marker Action write is reachable from WorldRuntime (#966)")
PF_ASSERT_UNREACHABLE(game::WorldRuntime&, setDoorBroken(core::TraversalResourceId{}, true),
	"a Door state write is reachable from WorldRuntime (#966)")
PF_ASSERT_UNREACHABLE(game::WorldRuntime&, setRandomSeed(0ull),
	"a seed write is reachable from WorldRuntime (#966)")
PF_ASSERT_UNREACHABLE(game::WorldRuntime&, createAgent(std::string{}),
	"Agent creation is reachable from WorldRuntime (#966)")
PF_ASSERT_UNREACHABLE(game::WorldRuntime&, invalidateSimulationSnapshot(),
	"snapshot invalidation is reachable from WorldRuntime: it writes, so it is not a read (#966)")
PF_ASSERT_UNREACHABLE(const game::WorldRuntime&, setLayerName(0u, std::string{}),
	"a Layer rename is reachable through a const WorldRuntime (#966)")
PF_ASSERT_UNREACHABLE(const game::WorldRuntime&, isModified(),
	"the document modified flag is reachable through a const WorldRuntime (#966)")

namespace
{
	using smoke::require;
	using game_runtime_smoke::makeFullReadSurfaceWorld;

	// The runtime and the const view are two names for one World, so a read
	// taken through each cannot be told apart. Value reads compare by value.
	#define PF_SAME_READ(...) \
		require((surface.__VA_ARGS__) == (view.__VA_ARGS__), \
			"read through the runtime differs from the same read through world(): " #__VA_ARGS__)

	// A read that hands back a reference is checked by identity instead: the
	// same object, not merely an equal copy. That is the stronger claim for a
	// forward, and it sidesteps types that define no operator==.
	#define PF_SAME_OBJECT(...) \
		require(&surface.__VA_ARGS__ == &view.__VA_ARGS__, \
			"read through the runtime returned a different object than the same read through world(): " #__VA_ARGS__)

	// A read that refuses an inapplicable argument must refuse identically
	// through the runtime: the forward adds no policy of its own, so the
	// message the World writes is the message the game gets. Each read is run
	// to a string - value or thrown message - and the two strings compared.
	template <typename Read>
	std::string outcome(Read read)
	{
		try { return read(); }
		catch (std::exception const& fault) { return std::string("threw: ") + fault.what(); }
	}

	// Entity ids are records wrapping a number rather than numbers, so the
	// joiner unwraps them where it can and falls back to the value itself.
	template <typename Value>
	std::string one(Value const& value)
	{
		if constexpr (requires { value.value; }) return std::to_string(value.value);
		else return std::to_string(value);
	}

	template <typename Values>
	std::string joined(Values const& values)
	{
		std::string text;
		for (auto const& value : values)
		{
			if (!text.empty()) text += ',';
			text += one(value);
		}
		return text;
	}

	template <typename RuntimeRead, typename ViewRead>
	void requireSameOutcome(RuntimeRead throughRuntime, ViewRead throughView,
		std::string_view label)
	{
		std::string const viaRuntime = outcome(std::move(throughRuntime));
		std::string const viaView = outcome(std::move(throughView));
		require(viaRuntime == viaView,
			"the runtime answered " + viaRuntime + " where the World answered " + viaView
				+ " for " + std::string(label));
	}

	// EntityLookup declares no operator==, so its three observable parts are
	// compared one by one.
	#define PF_SAME_LOOKUP(call) \
		do { \
			auto const viaRuntime = surface.call; \
			auto const viaView = view.call; \
			require(bool(viaRuntime) == bool(viaView), "lookup agreement broke: " #call); \
			require(viaRuntime.entity == viaView.entity, "lookup entity agreement broke: " #call); \
			require(viaRuntime.diagnostic == viaView.diagnostic, "lookup diagnostic agreement broke: " #call); \
		} while (false)

	// Everything a read could plausibly disturb, taken in one pass. The
	// snapshot view is read last so its mutable-cache rebuild sits inside the
	// fingerprint: rebuilding is the established const-read pattern, but a
	// read that *invalidated* the cache would show up as a build-count change
	// against the warmed baseline.
	struct Fingerprint
	{
		std::string name{};
		std::uint64_t tick{ 0 };
		std::uint64_t topologyGeneration{ 0 };
		std::uint64_t tagAssignments{ 0 };
		std::uint32_t sectors{ 0 };
		std::uint32_t agents{ 0 };
		std::uint64_t snapshotBuilds{ 0 };
		std::size_t events{ 0 };
		float agentX{ 0.0f };
		std::uint64_t snapshotTick{ 0 };

		bool operator==(Fingerprint const& other) const
		{
			return name == other.name && tick == other.tick
				&& topologyGeneration == other.topologyGeneration
				&& tagAssignments == other.tagAssignments && sectors == other.sectors
				&& agents == other.agents && snapshotBuilds == other.snapshotBuilds
				&& events == other.events && agentX == other.agentX
				&& snapshotTick == other.snapshotTick;
		}
	};

	Fingerprint fingerprint(game::WorldRuntime const& surface, core::AgentId agent)
	{
		Fingerprint state;
		state.name = surface.getName();
		state.tick = surface.getSimulationTick();
		state.topologyGeneration = surface.getTopologyGeneration();
		state.tagAssignments = surface.getAgentTagAssignmentCount();
		state.sectors = surface.getNumSectors();
		state.agents = surface.getSimulationSnapshot().agents.size();
		state.events = surface.getSimulationEvents().size();
		state.snapshotTick = surface.getSimulationSnapshotView().tick;
		state.snapshotBuilds = surface.getSimulationSnapshotBuildCount();
		if (auto const held = surface.lookupAgent(agent); held.entity != nullptr)
			state.agentX = held.entity->getGlobalPosition().x;
		return state;
	}

	// One pass over the forwarded surface, taken through a `const` reference
	// so the sweep itself cannot compile if any of these stops being a read.
	void sweep(game::WorldRuntime const& surface,
		core::AgentId agent, std::uint32_t corridor, core::MarkerId marker,
		core::InteractionPointId point, core::DeviceOperationId operation,
		core::TraversalResourceId resource, core::AgentGroupId group,
		core::AccessPermissionId permission, core::PermissionSetId set)
	{
		(void)surface.getName();
		(void)surface.getClipboardIdentity();
		(void)surface.getLifetimeToken();
		(void)surface.getRandomSeed();
		(void)surface.documentDirectory();
		(void)surface.getLoadWarnings();
		(void)surface.getBuildLog();
		(void)surface.getCellsWide();
		(void)surface.getLevelsHigh();
		(void)surface.getLayerCount();
		(void)surface.getLayerName(0);
		(void)surface.getLevelName(0);
		(void)surface.getNumSectors();
		(void)surface.getLayer(0);
		(void)surface.getGraph();
		(void)surface.getSector(corridor);
		(void)surface.getSectors(0);
		(void)surface.getSectorsInBounds(0, 0.0f, 0.0f, 16.0f, 4.0f);
		(void)surface.getSectorAtPosition(0, 1.0f, 0.5f);
		(void)surface.getAgentAtPosition(0, 0.5f, 0.5f);
		(void)surface.getObjectAtPosition(0, 0.5f, 0.5f);
		core::World::TopologyPathIntent intent{};
		(void)surface.getPausedPathIntent(*surface.lookupAgent(agent).entity, intent);
		(void)surface.isTraversalTopologyDirty();
		(void)surface.isTraversalTopologyValid();
		(void)surface.getTopologyGeneration();
		(void)surface.getTopologyDiagnostic();
		(void)surface.hasDumbwaiters();
		(void)surface.isControlTargetBroken(point);
		(void)surface.getMarkerIds();
		(void)surface.lookupMarker(marker);
		(void)surface.markerActions(marker);
		(void)surface.availableAgentActions(marker);
		(void)surface.agentActionDisplayName("greet");
		(void)surface.hasAgentType("human");
		(void)surface.agentTypeDisplayName("human");
		(void)surface.agentTypeResourceName("human");
		(void)surface.lookupAgent(agent);
		(void)surface.getAgentId(surface.lookupAgent(agent).entity);
		(void)surface.maximumInstalledStandingWidth();
		(void)surface.getAgentTags(agent);
		(void)surface.getAgentGroup(agent);
		(void)surface.getAgentGroupCount();
		(void)surface.getAgentGroupIds();
		(void)surface.lookupAgentGroup(group);
		(void)surface.getAgentGroupName(group);
		(void)surface.isAgentGroupActive(group);
		(void)surface.getAgentGroupMemberCount(group);
		(void)surface.getAccessPermissionCount();
		(void)surface.getAccessPermissionIds();
		(void)surface.lookupAccessPermission(permission);
		(void)surface.getAccessPermissionName(permission);
		(void)surface.getAccessPermissionUsage(permission);
		(void)surface.getAgentDirectAccessGrants(agent);
		(void)surface.getAgentCurrentDirectAccessGrants(agent);
		(void)surface.getAgentCurrentPermissionSetAssignments(agent);
		(void)surface.getAgentEffectiveAccessGrants(agent);
		(void)surface.getAgentAccessGrantSources(agent, permission);
		(void)surface.getPermissionSetCount();
		(void)surface.getPermissionSetIds();
		(void)surface.lookupPermissionSet(set);
		(void)surface.getPermissionSetName(set);
		(void)surface.getPermissionSetUsageCount(set);
		(void)surface.getPermissionSetPermissions(set);
		(void)surface.getAgentPermissionSetAssignments(agent);
		(void)surface.isLocationPermissionEligible(corridor);
		(void)surface.getLocationPermissionRequirement(corridor);
		(void)surface.isInteractionPointPermissionEligible(point);
		(void)surface.getInteractionPointPermissionRequirement(point);
		(void)surface.isAgentTransportOccupant(resource, agent);
		(void)surface.isAgentInQueue(agent);
		(void)surface.lookupInteractionPoint(point);
		(void)surface.lookupDeviceOperation(operation);
		(void)surface.lookupTraversalResource(resource);
		(void)surface.lookupInteractionRequest(core::InteractionRequestId{});
		(void)surface.lookupTraversalRequest(core::TraversalRequestId{});
		(void)surface.lookupBoothWindow(core::BoothWindowId{});
		(void)surface.lookupAccessPanel(core::AccessPanelId{});
		(void)surface.lookupDumbwaiter(core::DumbwaiterId{});
		(void)surface.getTraversalResourceId(nullptr);
		(void)surface.getRouteChoicePolicy();
		(void)surface.getTraversalWaitingPolicy();
		(void)surface.getTraversalGeometryPolicy();
		(void)surface.hasAgentTagRegistryReference();
		(void)surface.hasAttachedAgentTagRegistry();
		(void)surface.getExpectedAgentTagRegistryUuid();
		(void)surface.getAgentTagRegistry();
		(void)surface.getAgentTagAssignmentCount();
		(void)surface.getAgentTagAssignedAgentCount();
		(void)surface.getAgentTagSampleCount();
		(void)surface.hasAgentBehaviourRegistryReference();
		(void)surface.hasAttachedAgentBehaviourRegistry();
		(void)surface.getExpectedAgentBehaviourRegistryUuid();
		(void)surface.getAgentBehaviourRegistry();
		(void)surface.agentBehaviourConfigurationsAreValid();
		(void)surface.getAgentBehaviourDependencyDiagnostic();
		(void)surface.getAgentBehaviourAssignment(agent);
		(void)surface.getAgentBehaviourAssignmentCount();
		(void)surface.agentBehaviourOwnsMovement(agent);
		(void)surface.getAgentBehaviourRuntimeDiagnostics();
		(void)surface.getAgentBehaviourRuntimeLimits();
		(void)surface.actionRegistry();
		(void)surface.furnitureCatalogue();
		(void)surface.furniture();
		(void)surface.isFurnitureMarker(marker);
		(void)surface.usablePointOccupant(marker);
		(void)surface.getDeferredSimulationTime();
		(void)surface.getSimulationEvents();
		(void)surface.getSimulationSnapshot();
		(void)surface.getSimulationSnapshotView();
		(void)surface.getSimulationSnapshotBuildCount();
	}
}

void registerGameRuntimeReadSurface(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "runtimeReadsAgreeWithTheRenderView", [](smoke::Context const& ctx)
		{
			auto fixture = makeFullReadSurfaceWorld("agreement", ctx.temporaryRoot());
			game::WorldRuntime runtime(std::move(fixture.world));
			game::WorldRuntime const& surface = runtime;
			core::World const& view = runtime.world();

			// Identity, host state and diagnostics.
			PF_SAME_OBJECT(getName());
			PF_SAME_READ(getRandomSeed());
			PF_SAME_OBJECT(getClipboardIdentity());
			// std::weak_ptr declares no operator==, so the two tokens are
			// compared by owner: same owner, same World, same lifetime.
			auto const tokenRuntime = surface.getLifetimeToken();
			auto const tokenView = view.getLifetimeToken();
			require(!tokenRuntime.owner_before(tokenView) && !tokenView.owner_before(tokenRuntime),
				"the lifetime token read through the runtime names a different owner than the render view's");
			PF_SAME_OBJECT(getLoadWarnings());
			PF_SAME_OBJECT(getBuildLog());
			PF_SAME_OBJECT(documentDirectory());

			// Dimensions, Layers, Levels and the traversal graph.
			PF_SAME_READ(getCellsWide());
			PF_SAME_READ(getLevelsHigh());
			PF_SAME_READ(getLayerCount());
			PF_SAME_OBJECT(getLayerName(0));
			PF_SAME_OBJECT(getLevelName(0));
			PF_SAME_READ(getNumSectors());
			PF_SAME_READ(getLayer(0));
			PF_SAME_READ(getGraph());

			// Sectors and spatial lookup.
			PF_SAME_READ(getSector(fixture.corridor));
			PF_SAME_READ(getSectors(0));
			PF_SAME_READ(getSectorsInBounds(0, 0.0f, 0.0f, 16.0f, 4.0f));
			PF_SAME_READ(getSectorAtPosition(0, 1.0f, 0.5f));
			PF_SAME_READ(getAgentAtPosition(0, 0.5f, 0.5f));
			PF_SAME_READ(getObjectAtPosition(0, 0.5f, 0.5f));

			// Topology state.
			PF_SAME_READ(isTraversalTopologyDirty());
			PF_SAME_READ(isTraversalTopologyValid());
			PF_SAME_READ(getTopologyGeneration());
			PF_SAME_OBJECT(getTopologyDiagnostic());

			// Markers and Agent actions.
			PF_SAME_READ(getMarkerIds());
			PF_SAME_READ(lookupMarker(fixture.marker));
			PF_SAME_READ(markerActions(fixture.marker));
			PF_SAME_READ(availableAgentActions(fixture.marker));
			PF_SAME_READ(agentActionDisplayName("greet"));

			// Agent types and Agents.
			PF_SAME_READ(hasAgentType("human"));
			PF_SAME_READ(agentTypeDisplayName("human"));
			PF_SAME_LOOKUP(lookupAgent(fixture.agent));
			PF_SAME_READ(getAgentId(view.lookupAgent(fixture.agent).entity));
			PF_SAME_READ(maximumInstalledStandingWidth());

			// Referenced packages: the fixture attached both registries, so
			// these read the reference rather than faulting on its absence.
			PF_SAME_READ(hasAgentTagRegistryReference());
			PF_SAME_READ(hasAttachedAgentTagRegistry());
			PF_SAME_OBJECT(getExpectedAgentTagRegistryUuid());
			PF_SAME_OBJECT(getAgentTagRegistry());
			PF_SAME_READ(hasAgentBehaviourRegistryReference());
			PF_SAME_READ(hasAttachedAgentBehaviourRegistry());
			PF_SAME_OBJECT(getExpectedAgentBehaviourRegistryUuid());
			PF_SAME_OBJECT(getAgentBehaviourRegistry());

			// Agent groups and tags.
			PF_SAME_READ(getAgentGroup(fixture.agent));
			PF_SAME_READ(getAgentGroupCount());
			PF_SAME_READ(getAgentGroupIds());
			PF_SAME_LOOKUP(lookupAgentGroup(fixture.group));
			PF_SAME_OBJECT(getAgentGroupName(fixture.group));
			PF_SAME_READ(isAgentGroupActive(fixture.group));
			PF_SAME_READ(getAgentGroupMemberCount(fixture.group));
			PF_SAME_OBJECT(getAgentTags(fixture.agent));
			PF_SAME_READ(getAgentTagAssignmentCount());
			PF_SAME_READ(getAgentTagAssignedAgentCount());
			PF_SAME_READ(getAgentTagSampleCount());

			// Permissions, permission sets and grants.
			PF_SAME_READ(getAccessPermissionCount());
			PF_SAME_READ(getAccessPermissionIds());
			PF_SAME_LOOKUP(lookupAccessPermission(fixture.permission));
			PF_SAME_OBJECT(getAccessPermissionName(fixture.permission));
			// AccessPermissionUsage is a plain count record with no
			// operator==, so each count is compared against its own.
			auto const usageRuntime = surface.getAccessPermissionUsage(fixture.permission);
			auto const usageView = view.getAccessPermissionUsage(fixture.permission);
			require(usageRuntime.directAgentGrants == usageView.directAgentGrants
				&& usageRuntime.permissionSetMemberships == usageView.permissionSetMemberships
				&& usageRuntime.interactionPointRequirements == usageView.interactionPointRequirements
				&& usageRuntime.manualDoorRequirements == usageView.manualDoorRequirements
				&& usageRuntime.liftDestinationRequirements == usageView.liftDestinationRequirements
				&& usageRuntime.locationRequirements == usageView.locationRequirements,
				"the Access permission usage read through the runtime differs from the same read through world()");
			PF_SAME_READ(getAgentDirectAccessGrants(fixture.agent));
			PF_SAME_READ(getAgentEffectiveAccessGrants(fixture.agent));
			PF_SAME_READ(getAgentPermissionSetAssignments(fixture.agent));
			PF_SAME_READ(getPermissionSetCount());
			PF_SAME_LOOKUP(lookupPermissionSet(fixture.permissionSet));
			PF_SAME_OBJECT(getPermissionSetName(fixture.permissionSet));
			PF_SAME_READ(getPermissionSetPermissions(fixture.permissionSet));
			PF_SAME_READ(isLocationPermissionEligible(fixture.corridor));
			PF_SAME_READ(getLocationPermissionRequirement(fixture.corridor));
			PF_SAME_READ(isInteractionPointPermissionEligible(fixture.interactionPoint));
			requireSameOutcome(
				[&] { return std::to_string(surface.isManualDoorPermissionEligible(
					fixture.traversalResource)); },
				[&] { return std::to_string(view.isManualDoorPermissionEligible(
					fixture.traversalResource)); },
				"isManualDoorPermissionEligible");
			requireSameOutcome(
				[&] { return joined(surface.getManualDoorPermissionRequirement(
					fixture.traversalResource)); },
				[&] { return joined(view.getManualDoorPermissionRequirement(
					fixture.traversalResource)); },
				"getManualDoorPermissionRequirement");

			// Transport state and authorization. The fixture's bare
			// TraversalResource is not a transport, so the Stop and condition
			// reads refuse it - and refuse it the same way through both doors.
			requireSameOutcome(
				[&] { return surface.getTransportStopShortName(fixture.traversalResource, 0); },
				[&] { return view.getTransportStopShortName(fixture.traversalResource, 0); },
				"getTransportStopShortName");
			requireSameOutcome(
				[&] { return joined(surface.getLiftDestinationLevels(fixture.corridor)); },
				[&] { return joined(view.getLiftDestinationLevels(fixture.corridor)); },
				"getLiftDestinationLevels");
			requireSameOutcome(
				[&] {
					return std::to_string(surface.knownTransportCondition(
						fixture.traversalResource, nullptr, nullptr).has_value());
				},
				[&] {
					return std::to_string(view.knownTransportCondition(
						fixture.traversalResource, nullptr, nullptr).has_value());
				},
				"knownTransportCondition");
			requireSameOutcome(
				[&] {
					return std::to_string(surface.knownLiftCondition(
						fixture.traversalResource, nullptr, nullptr).has_value());
				},
				[&] {
					return std::to_string(view.knownLiftCondition(
						fixture.traversalResource, nullptr, nullptr).has_value());
				},
				"knownLiftCondition");
			PF_SAME_READ(isAgentTransportOccupant(fixture.traversalResource, fixture.agent));
			PF_SAME_READ(isAgentInQueue(fixture.agent));

			// Interaction points, devices and traversal resources.
			PF_SAME_LOOKUP(lookupInteractionPoint(fixture.interactionPoint));
			PF_SAME_LOOKUP(lookupDeviceOperation(fixture.deviceOperation));
			PF_SAME_LOOKUP(lookupTraversalResource(fixture.traversalResource));
			PF_SAME_READ(getTraversalResourceId(nullptr));

			// Routing policy, behaviour packages and Furniture.
			PF_SAME_OBJECT(getRouteChoicePolicy());
			PF_SAME_OBJECT(getTraversalWaitingPolicy());
			PF_SAME_OBJECT(getTraversalGeometryPolicy());
			PF_SAME_READ(agentBehaviourConfigurationsAreValid());
			PF_SAME_OBJECT(getAgentBehaviourDependencyDiagnostic());
			PF_SAME_OBJECT(getAgentBehaviourAssignment(fixture.agent));
			PF_SAME_READ(getAgentBehaviourAssignmentCount());
			PF_SAME_READ(agentBehaviourOwnsMovement(fixture.agent));
			PF_SAME_OBJECT(actionRegistry());
			PF_SAME_OBJECT(furnitureCatalogue());
			PF_SAME_OBJECT(furniture());
			PF_SAME_READ(isFurnitureMarker(fixture.marker));
			PF_SAME_READ(usablePointOccupant(fixture.marker));

			// Simulation clock reads.
			PF_SAME_READ(getDeferredSimulationTime());
			PF_SAME_OBJECT(getSimulationEvents());
			PF_SAME_READ(getSimulationSnapshotBuildCount());
		} });

	checks.push_back({ "runtimeReadsLeaveNoTraceOnTheWorld", [](smoke::Context const& ctx)
		{
			auto fixture = makeFullReadSurfaceWorld("purity", ctx.temporaryRoot());
			game::WorldRuntime runtime(std::move(fixture.world));
			game::WorldRuntime const& surface = runtime;
			require(surface.lookupAgent(fixture.agent).entity != nullptr,
				"the fixture Agent is missing, so the purity fingerprint would be meaningless");

			// Warm the lazily built snapshot before baselining so the first
			// build is not counted as a change the sweep caused.
			(void)surface.getSimulationSnapshot();
			auto const before = fingerprint(surface, fixture.agent);

			for (int pass = 0; pass < 3; ++pass)
				sweep(surface, fixture.agent, fixture.corridor, fixture.marker,
					fixture.interactionPoint, fixture.deviceOperation, fixture.traversalResource,
					fixture.group, fixture.permission, fixture.permissionSet);

			auto const after = fingerprint(surface, fixture.agent);
			require(before == after,
				"a read through the runtime changed observable World state: the read surface is not a read");
		} });

	checks.push_back({ "runtimeReadsFollowTheDrivenRun", [](smoke::Context const& ctx)
		{
			auto fixture = makeFullReadSurfaceWorld("follows-run", ctx.temporaryRoot());
			game::WorldRuntime runtime(std::move(fixture.world));
			game::WorldRuntime const& surface = runtime;

			auto const startTick = surface.getSimulationTick();
			auto const startX = surface.lookupAgent(fixture.agent).entity->getGlobalPosition().x;
			require(runtime.advanceTicks(60), "the run stopped before the reads could re-read");

			auto const endTick = surface.getSimulationTick();
			auto const endX = surface.lookupAgent(fixture.agent).entity->getGlobalPosition().x;
			require(endTick == startTick + 60,
				"the tick read through the runtime did not follow the driven run");
			require(endX > startX + 0.01f,
				"the Agent read through the runtime did not move with the driven run");

			// The static and structural reads are stable across the run: the
			// surface does not drift as the World advances.
			require(surface.getCellsWide() == 16u && surface.getLevelsHigh() == 4u,
				"the World dimensions read differently through the runtime after the run");
			require(surface.lookupInteractionPoint(fixture.interactionPoint)
				&& surface.lookupDeviceOperation(fixture.deviceOperation)
				&& surface.lookupTraversalResource(fixture.traversalResource),
				"an entity lookup stopped resolving through the runtime across the run");
			require(surface.getSimulationSnapshotView().tick == endTick,
				"the snapshot view read through the runtime is not on the current Tick");
		} });

	checks.push_back({ "runtimeReadsDiagnoseStaleHandlesWithoutFaulting", [](smoke::Context const& ctx)
		{
			auto fixture = makeFullReadSurfaceWorld("stale-handles", ctx.temporaryRoot());
			game::WorldRuntime runtime(std::move(fixture.world));
			game::WorldRuntime const& surface = runtime;

			auto const point = surface.lookupInteractionPoint(core::InteractionPointId{ 987654 });
			require(!point && point.entity == nullptr,
				"a stale InteractionPoint handle resolved through the runtime");
			require(!point.diagnostic.empty(),
				"the runtime rejected a stale InteractionPoint without a diagnostic");

			auto const agent = surface.lookupAgent(core::AgentId{ 987654 });
			require(!agent && agent.entity == nullptr,
				"a stale Agent handle resolved through the runtime");
			require(!agent.diagnostic.empty(),
				"the runtime rejected a stale Agent without a diagnostic");

			// A read of something the fixture simply does not have answers
			// "nothing", rather than faulting a game or render path.
			require(surface.lookupMarker(core::MarkerId{ 987654 }) == nullptr,
				"a stale Marker resolved through the runtime");
			require(!surface.isFurnitureMarker(fixture.marker),
				"the fixture Marker reads as Furniture when none was placed");
			require(surface.markerActions(fixture.marker).size() == 2,
				"the Marker Actions authored into the fixture are not what the runtime reads back");
		} });
}
