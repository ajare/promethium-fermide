#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "core/AgentType.h"
#include "core/World.h"

namespace core
{
	// Owns the behaviour of the World's registered Agent-type family on
	// behalf of the World that owns it (ADR 0022).
	//
	// Agent-type definitions are authored World data, not simulation state
	// (ADR 0006): the bundled Human is registered at construction and
	// additional `.agent.lua` types are attached through the public
	// workflow. The source is shared and immutable; executed instance
	// state lives in the World's AgentTypeRuntimeAdapter, which this
	// catalog never touches.
	//
	// This is the Agent-type behaviour moved out of World: bundled
	// registration, attachment with preflight validation and Stairwell
	// geometry guarding, resolution of saved type records (including
	// reconstruction against the current managed revision), unused-type
	// detachment, and the display-name / resource-name / cached-preview-
	// baseline queries the editor reads. Every operation is judged before
	// anything is written, so a refusal leaves the World exactly as it was
	// found.
	//
	// World keeps ownership of the Agent-type registry itself (ADR 0001)
	// and remains the facade (design pattern) through which callers reach
	// these operations; no caller outside World names this class. The
	// catalog owns no state: it reads and edits the World's mAgentTypes and
	// preview-baseline cache, and reaches the Agent and Sector registries,
	// the pause gate, and the topology rebuild seam through the World it
	// was given, following the SimulationCoordinator, PlacementValidator,
	// and AgentGroupRegistry pattern.
	class AgentTypeCatalog
	{
	public:

		explicit AgentTypeCatalog(World& world);

		AgentTypeCatalog(AgentTypeCatalog const&) = delete;
		AgentTypeCatalog& operator=(AgentTypeCatalog const&) = delete;

		virtual ~AgentTypeCatalog() = default;

		// ------------------------------------------------------------------
		// Registration
		// ------------------------------------------------------------------

		// Registers the bundled Human type so ordinary World creation, legacy
		// document loading, and candidate Worlds can resolve Human without an
		// editor startup. Runs once during construction; a no-op when Human is
		// already registered.
		void registerBundledAgentTypes();

		// Registers a `.agent.lua` type definition under `resourceName`. The
		// source is validated (type ID, display name, new()) before it is
		// accepted; a duplicate type ID or an invalid definition is refused
		// without mutating the World. A type whose Standing width would widen
		// Stairwell platforms is refused unless the simulation is paused and
		// no Agent occupies one; a failed topology rebuild rolls the
		// registration back. Returns false with a diagnostic on refusal.
		bool attachAgentType(std::string resourceName, std::string source,
			std::string* diagnostic = nullptr);

		// Roll back an unused placement registration; never detach Human or a
		// definition still owned by an Agent.
		bool detachUnusedAgentType(std::string_view typeId);

		// ------------------------------------------------------------------
		// Resolution
		// ------------------------------------------------------------------

		// Resolves a type ID (and optional explicit resource reference) to its
		// registered definition, or throws with a precise diagnostic. An
		// explicit resource reference is resolved through the installed loader
		// (or manifest catalog) and registered on first use; explicit
		// missing/mismatched references never silently fall back to Human.
		// Competing resources that declare an already-registered type ID are
		// rejected. Reconstruction additionally resolves the current managed
		// source revision; ordinary creation keeps the registered immutable
		// definition.
		std::shared_ptr<const AgentTypeDefinition> resolveAgentType(
			std::string const& typeId, std::string const& resourceName,
			bool reconstruct = false);

		// ------------------------------------------------------------------
		// Queries
		// ------------------------------------------------------------------

		// True when `typeId` names a registered Agent type in this World.
		bool hasAgentType(std::string_view typeId) const;

		// The display name of a registered type, or empty when unknown.
		std::string agentTypeDisplayName(std::string_view typeId) const;
		std::string agentTypeResourceName(std::string_view typeId) const;

		// Reads the World-owned immutable definition and its cached
		// construction baseline. Editor placement previews use these rather
		// than resolving a newer on-disk resource revision.
		std::optional<AgentTypeDefinition> registeredAgentTypeDefinition(
			std::string_view typeId, std::string_view resourceName) const;
		std::optional<AgentPhysicalBaseline> registeredAgentTypeBaseline(
			std::string_view typeId, std::string_view resourceName) const;

	private:

		// The catalog owns nothing: it reads and edits the World's Agent-type
		// state through this reference (ADR 0001/0022).
		World& mWorld;
	};
}
