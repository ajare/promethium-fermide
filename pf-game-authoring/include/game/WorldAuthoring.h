#pragma once

#include "core/World.h"
#include "game/AuthoringPlatform.h"
#include "game/WorldRuntime.h"

namespace game
{
	// The authoring half of the game-facing boundary (design pattern) over a
	// World (#967, spec #960). Where `WorldRuntime` owns a World and is the
	// only way to *drive* it, `WorldAuthoring` is the only way to *change* it.
	//
	// It wraps a `WorldRuntime` rather than owning a World beside it. That is
	// not a contrivance: it is what the editor already does, live-editing
	// against a running simulation. Because authoring sits over the runtime,
	// the editor's preview is literally the game's runtime path with authoring
	// powers added, rather than a second implementation of it that can drift.
	//
	// The privilege is expressed in the types rather than left to discipline.
	// `WorldRuntime` hands out `core::World const&` and nothing else; the
	// mutable view it holds is private and granted to this class alone, so
	// `WorldAuthoring` really is the only thing in a process that can turn a
	// running World into a mutable one. A program that links only
	// `pf-game-runtime` cannot even name this type, which is why the game's
	// link line is the review-visible statement of what it is allowed to do.
	//
	// This is the external, consumer-facing authoring surface. It is not
	// `core::WorldBuilder`, the internal collaborator `World` forwards to
	// (ADR 0022); authoring reaches `WorldBuilder` only through `World`,
	// exactly as every other caller does.
	//
	// The tracer bullet is the wrapping itself: the mutable `World&` for
	// authoring and the wrapped runtime handed back for preview. The authoring
	// method families - topology and preflight (#968), resource binding and
	// persistence (#969) - are later tickets and are deliberately not here yet.
	class PF_GAME_AUTHORING_API WorldAuthoring
	{
	public:
		// Wraps `runtime`. Nothing is owned and nothing is copied: the World
		// stays the runtime's, and the runtime must outlive this handle. Taking
		// a reference rather than a pointer makes "authoring needs a running
		// World to author against" a compile-time fact - there is no null case
		// to refuse at runtime.
		explicit WorldAuthoring(WorldRuntime& runtime) noexcept;

		~WorldAuthoring();

		// The handle is movable but never copied: copying would suggest two
		// independently held privileges where there is only one view of one
		// World. A moved-from handle holds nothing and must not be used again.
		WorldAuthoring(WorldAuthoring&& other) noexcept;
		WorldAuthoring& operator=(WorldAuthoring&& other) noexcept;
		WorldAuthoring(WorldAuthoring const&) = delete;
		WorldAuthoring& operator=(WorldAuthoring const&) = delete;

		// The mutable World, for authoring. This is the only mutable `World&`
		// reachable through the game-facing boundary, and it is valid only for
		// as long as the wrapped runtime lives. Structural edits follow the
		// protocol the World already enforces: pause at the edit/simulation
		// boundary, edit, rebuild, resume.
		core::World& world() noexcept;

		// The wrapped runtime, for preview. The editor renders and drives
		// through this, so what it previews is the same runtime path the game
		// runs - the guarantee this arrangement exists to provide.
		WorldRuntime& runtime() noexcept;

		WorldRuntime const& runtime() const noexcept;

	private:
		// Not owned. The runtime owns the World; this only names it.
		WorldRuntime* runtime_;
	};
}
