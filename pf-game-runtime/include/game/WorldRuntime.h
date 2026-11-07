#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/Simulation.h"
// Pulled in whole rather than forward declared: the transport Stop selector is a
// nested type of `core::World`, and #960 decision 6 keeps the read-model types
// as boundary types instead of mirroring them, so the runtime returns the core
// struct itself rather than a second copy that could drift.
#include "core/World.h"
#include "game/Platform.h"

namespace game
{
	// The game-facing façade (design pattern) over a World: it owns one World
	// exclusively and is the only way to drive it (#964, spec #960).
	//
	// Exclusive ownership is the whole point of the type. The World is held as a
	// `std::unique_ptr`, the runtime is not copyable, and the only public World
	// accessor hands out a `core::World const&`. A render path therefore
	// cannot mutate what it renders, and no second holder of the same World can
	// exist. Authoring lives over the runtime rather than beside it: the
	// privileged mutable view arrives with WorldAuthoring (#967), never here.
	//
	// Everything below is a thin forward onto the same simulation the editor
	// already drives, so behaviour is unchanged; only the shape of the surface
	// through which a game reaches it is new.
	class PF_GAME_RUNTIME_API WorldRuntime
	{
	public:
		// Takes exclusive ownership of `world`. Throws std::invalid_argument
		// when `world` is null: a runtime with nothing to drive is not a state
		// a caller may construct.
		explicit WorldRuntime(std::unique_ptr<core::World> world);

		// Convenience construction of the owned World with the same accepted
		// dimensions as the core constructor.
		WorldRuntime(std::string const& name, std::uint32_t cellsWide, std::uint32_t levelsHigh);

		~WorldRuntime();

		// Exclusive ownership transfers; it is never shared or copied. A moved-from
		// runtime holds nothing and must not be driven again: it is a shell whose
		// only purpose is to have handed the World over.
		WorldRuntime(WorldRuntime&& other) noexcept;
		WorldRuntime& operator=(WorldRuntime&& other) noexcept;
		WorldRuntime(WorldRuntime const&) = delete;
		WorldRuntime& operator=(WorldRuntime const&) = delete;

		// The read view handed to rendering. Valid for as long as this runtime
		// lives and remains the same World throughout; every read through it is
		// const, so rendering cannot mutate what it renders.
		core::World const& world() const noexcept;

		// --- Driving -------------------------------------------------------
		// Rendering supplies elapsed wall time here. It is accumulated and only
		// whole fixed simulation ticks are executed, exactly as on World.
		void update(float elapsedSeconds);

		// Headless deterministic seam. Never uses render timing; reports false
		// when a behaviour failure stops the run before the next tick.
		bool advanceTick();

		bool advanceTicks(std::uint64_t count);

		// Pause, resume and reset behave exactly as they do on World today:
		// pausing cancels live traversal while retaining each Agent's route
		// intent, resuming over dirty topology is refused, and reset restores
		// the authored state.
		void pauseSimulation();

		bool resumeSimulation();

		void resetSimulation();

		bool isSimulationPaused() const noexcept;

		// Release every Agent the World owns without reaching into Agent
		// internals.
		void wakeAllAgents();

		void setSimulationTimeScale(double scale);

		double getSimulationTimeScale() const noexcept;

		// Structural build steps the runtime lifecycle owns: finishBuild()
		// generates the traversal graph for a freshly created World, and
		// rebuildTraversalTopology() revalidates it after a paused edit.
		void finishBuild();

		bool rebuildTraversalTopology();

		// --- Player input --------------------------------------------------
		// The whole surface a player drives a World through (#965, spec #960).
		// Every one of these is held by the World as transient input state: it is
		// never authored, never persisted, and is read once per fixed Tick. Each
		// method forwards to the same handling the editor's play mode already
		// uses, so behaviour is identical; only the route to it is new.
		//
		// Input is dropped while the simulation is paused, exactly as on World:
		// a paused World cannot be steered, so a held key does not fire on the
		// first Tick after a resume.

		// The Agent the input belongs to: the lowest stable-ID Agent whose
		// behaviour assignment makes it player-controlled. A default-constructed
		// AgentId means nothing is controlled, and every other method here then
		// has nothing to act on.
		core::AgentId getPlayerControlledAgent() const;

		// Held steering: -1 left, 0 idle, +1 right, clamped on the way in.
		void setPlayerMovementDirection(int direction);

		// Held Staircase/Stairwell choice. Both set at once is read as no
		// selection rather than as a conflict, and neither key alone selects a
		// flight the Player is not walking into.
		void setPlayerStairSelection(bool up, bool down);

		// Held crouch: the Player lowers to Crouching standing still and to
		// Crawling while moving, and only where the space around them takes it.
		void setPlayerCrouch(bool held);

		// One press of the contextual action key, consumed at the next
		// allocation phase. Its meaning is read from where the Player stands:
		// a threshold crossing first, then opening a manual Door, then the
		// nearest physical control.
		void requestPlayerDoorAction();

		// The Stop list a Player riding a Lift or Shuttle is shown, or
		// std::nullopt when they are not in a position to choose one.
		std::optional<core::World::PlayerTransportSelector> getPlayerTransportSelector() const;

		// Moves the highlighted Stop in the selector; `horizontal` browses a
		// Shuttle's side-by-side Stops rather than a Lift's stacked ones. It
		// changes the highlight only, nothing is requested until confirm.
		void browsePlayerTransportStop(int direction, bool horizontal);

		// Requests the highlighted Stop for the riding Player. Repeating it on
		// the same Stop is idempotent, and the request outlives the Player
		// stepping off.
		void confirmPlayerTransportStop();

		// Drops every held and pending input at once. The Agent stops on the
		// next Tick rather than coasting to a halt, and a pressed-but-unconsumed
		// action is discarded rather than delivered later.
		void clearPlayerInput();

		// --- Observation ---------------------------------------------------
		std::uint64_t getSimulationTick() const noexcept;

		core::SimulationPhase getCurrentSimulationPhase() const noexcept;

		core::SimulationSnapshot getSimulationSnapshot() const;

		// Valid until the next tick or mutation; callers retaining data must
		// copy it.
		core::SimulationSnapshot const& getSimulationSnapshotView() const;

		std::uint64_t getSimulationSnapshotBuildCount() const noexcept;

		// Drains the event queue: each event is returned exactly once.
		std::vector<core::SimulationEvent> consumeSimulationEvents();

		// Non-owning; register/unregister on the simulation thread. The
		// observer must outlive its registration and may only read the World
		// during callbacks.
		void setSimulationObserver(core::SimulationObserver* observer) noexcept;

		// The fixed simulation timestep and the per-update tick bound, mirrored
		// from core so a host can pace itself without including core/World.h.
		static float getFixedTimestep();

		static std::uint32_t getMaxTicksPerUpdate();

	private:
		// Exclusive ownership. Nothing else in the process holds this World: the
		// runtime is not copyable, no shared_ptr is ever formed from it, and the
		// only public accessor is the const view above.
		std::unique_ptr<core::World> world_;

		// The mutable view of the owned World. Private, and granted to
		// WorldAuthoring alone (#967): ownership stays here, while the authoring
		// handle wrapped over the runtime is the only code in the process that
		// can turn this World back into a mutable one. Absent that grant there
		// is no path from a WorldRuntime to a `core::World&` at all, which is
		// what makes "authoring is the only mutable surface" a compile-time
		// fact rather than a convention - and why a game linking only
		// pf-game-runtime cannot reach mutable World state by any route.
		core::World& authoringWorld() noexcept;

		friend class WorldAuthoring;
	};
}
