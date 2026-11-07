#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/Simulation.h"
#include "game/Platform.h"

namespace core
{
	class World;
}

namespace game
{
	// The game-facing façade (design pattern) over a World: it owns one World
	// exclusively and is the only way to drive it (#964, spec #960).
	//
	// Exclusive ownership is the whole point of the type. The World is held as a
	// `std::unique_ptr`, the runtime is not copyable, and the only World
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
		// only accessor is the const view above.
		std::unique_ptr<core::World> world_;
	};
}
