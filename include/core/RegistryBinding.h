#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include "core/RegistryReference.h"
#include "core/World.h"

namespace core
{
	// A deferred, move-only-capable commit step. The behaviour family's
	// preflight closures own their candidate runtime in a unique_ptr, which
	// std::function cannot hold; the binding only needs to move the closure
	// along and call it once at the shared commit point. An empty hook is a
	// no-op, for families with nothing to swap.
	class CommitHook
	{
	public:
		CommitHook() = default;

		template <typename Fn,
			typename = std::enable_if_t<!std::is_same_v<
				std::decay_t<Fn>, CommitHook>>>
		CommitHook(Fn&& fn)
			: mImpl(std::make_shared<Impl<std::decay_t<Fn>>>(
				std::forward<Fn>(fn)))
		{
		}

		void operator()() const
		{
			if (mImpl) mImpl->call();
		}

		explicit operator bool() const { return mImpl != nullptr; }

	private:
		struct ImplBase
		{
			virtual ~ImplBase() = default;
			virtual void call() = 0;
		};

		template <typename Fn>
		struct Impl final : ImplBase
		{
			explicit Impl(Fn fn) : mFn(std::move(fn)) {}
			void call() override { mFn(); }
			Fn mFn;
		};

		std::shared_ptr<ImplBase> mImpl;
	};

	// One parameterised binding over a registry type plus its resource-name/
	// UUID pair (ADR 0022, #676): the shared attach / detach / resolve /
	// replace / has / get state machine that the Agent tag and Agent
	// behaviour families previously implemented twice over ~23 near-identical
	// World methods.
	//
	// The binding owns no state. The reference pair and the registry pointer
	// stay World-owned members (ADR 0001); the binding operates on them
	// through the references it was constructed with, so direct writes by
	// serialization and construction-record replay remain authoritative.
	// Type-specific validation, clearing, and runtime handling arrive through
	// the Policy callbacks the owning World configures. The binding keeps the
	// shared ordering contract: every refusal happens before any authored
	// state changes, the replacement registry is registered before the
	// previous one is unregistered, and the swap commits as one operation.
	//
	// World instantiates one binding per registry family and its facade
	// methods forward here; no caller outside World names this class.
	template <typename Registry>
	class RegistryBinding
	{
	public:
		using Reference = RegistryReference<Registry>;

		// The type-specific steps of the shared state machine. A hook refuses
		// an operation by throwing before the binding has changed any state.
		// Hooks that preflight a replacement return the commit closure the
		// binding runs after the replacement registry is registered and
		// before the previous one is unregistered (the behaviour family's
		// runtime swap); a family with nothing to swap returns an empty
		// closure.
		struct Policy
		{
			// Shared diagnostic wording, e.g. "Agent tag registry".
			std::string label;

			// Pause gates. Empty for a family whose transaction is not
			// paused-only; each throws while the simulation is running.
			std::function<void()> requirePausedForAttach;
			std::function<void()> requirePausedForAttachAndClear;
			std::function<void()> requirePausedForDetach;
			std::function<void()> requirePausedForReplaceCopy;

			// attach: the family's guards and preflight (namespace use,
			// assignment validation, candidate runtime preparation).
			std::function<CommitHook(Registry& registry,
				bool sameNamespace)> preflightAttach;
			// Runs after the reference/registry pair has been swapped in.
			std::function<void()> afterAttach;
			// True dirties the World only when the persisted reference
			// changed (behaviour); false dirties unconditionally (tag).
			bool modifyOnlyIfReferenceChanged{ false };

			// attachAndClearAssignments: the confirmed destructive switch.
			// The tag family keeps the identical-reattach no-op; the
			// behaviour family must not skip its clearing.
			bool attachAndClearIdenticalIsNoOp{ true };
			std::function<CommitHook(Registry& registry)>
				preflightAttachAndClear;
			std::function<void()> afterAttachAndClear;

			// detach: throws while authored state still depends on the
			// namespace.
			std::function<void()> guardDetach;
			// Runs after the detached namespace has been reset.
			std::function<void()> afterDetach;

			// detachAndClearAssignments: clears every assignment (and
			// samples) before the previous registry is unregistered, and
			// returns the commit closure run afterwards (runtime swap).
			std::function<CommitHook()> clearAssignmentsBeforeDetach;

			// resolve: the whole type-specific body after the shared UUID
			// prologue, with the World's registry slot handed over for the
			// swap.
			std::function<void(std::shared_ptr<Registry> registry,
				std::shared_ptr<Registry>& registrySlot)> resolveBody;

			// replaceWithIndependentCopy: attached-source, new-UUID,
			// definition-equivalence, and runtime checks; throws to refuse.
			std::function<CommitHook(std::string const& resourceName,
				std::shared_ptr<Registry> const& replacement)>
				preflightReplaceCopy;
			std::function<void()> afterReplaceCopy;

			// markUnavailable: records the dependency diagnostic and pauses
			// the simulation (behaviour family only).
			std::function<void(Reference const& reference,
				std::string diagnostic)> markUnavailable;
		};

		RegistryBinding(World& world, Policy policy,
			std::optional<Reference>& reference,
			std::shared_ptr<Registry>& registry)
			: mWorld(world)
			, mPolicy(std::move(policy))
			, mReference(reference)
			, mRegistry(registry)
		{
		}

		RegistryBinding(RegistryBinding const&) = delete;
		RegistryBinding& operator=(RegistryBinding const&) = delete;

		// ------------------------------------------------------------------
		// Reference and attachment queries
		// ------------------------------------------------------------------

		bool hasReference() const
		{
			return mReference.has_value();
		}

		bool hasAttachedRegistry() const
		{
			return mRegistry != nullptr;
		}

		// Throw while the World references no registry Resource.
		std::string const& resourceName() const
		{
			if (!mReference)
				throw std::runtime_error("The World has no " + mPolicy.label
					+ " reference");
			return mReference->resourceName;
		}

		std::string const& expectedUuid() const
		{
			if (!mReference)
				throw std::runtime_error("The World has no " + mPolicy.label
					+ " reference");
			return mReference->expectedUuid;
		}

		std::shared_ptr<Registry> const& registry() const
		{
			return mRegistry;
		}

		// ------------------------------------------------------------------
		// Attach / detach
		// ------------------------------------------------------------------

		// A non-destructive switch: refused while authored state depends on
		// the current namespace. Re-attaching the identical Resource name,
		// UUID, and registry object is a no-op.
		void attach(std::string resourceName, std::shared_ptr<Registry> registry)
		{
			mWorld.invalidateSimulationSnapshot();
			if (mPolicy.requirePausedForAttach) mPolicy.requirePausedForAttach();
			requireResourceName(resourceName);
			requireValidRegistry("Cannot attach an invalid " + mPolicy.label,
				registry);
			if (identicalAttached(resourceName, registry)) return;
			auto const commit = mPolicy.preflightAttach(*registry,
				sameNamespace(*registry));
			commitSwap(std::move(resourceName), std::move(registry), commit,
				mPolicy.afterAttach, mPolicy.modifyOnlyIfReferenceChanged);
		}

		// The explicit destructive switch: clears every assignment (and
		// samples) in the same paused-only registry-reference transaction,
		// used after editor confirmation. Registry documents themselves are
		// never changed.
		void attachAndClearAssignments(std::string resourceName,
			std::shared_ptr<Registry> registry)
		{
			mWorld.invalidateSimulationSnapshot();
			if (mPolicy.requirePausedForAttachAndClear)
				mPolicy.requirePausedForAttachAndClear();
			requireResourceName(resourceName);
			requireValidRegistry("Cannot attach an invalid " + mPolicy.label,
				registry);
			if (mPolicy.attachAndClearIdenticalIsNoOp
				&& identicalAttached(resourceName, registry)) return;
			auto const commit = mPolicy.preflightAttachAndClear(*registry);
			commitSwap(std::move(resourceName), std::move(registry), commit,
				mPolicy.afterAttachAndClear, false);
		}

		void detach()
		{
			mWorld.invalidateSimulationSnapshot();
			if (!mReference) return;
			if (mPolicy.requirePausedForDetach) mPolicy.requirePausedForDetach();
			if (mPolicy.guardDetach) mPolicy.guardDetach();
			if (mRegistry) mRegistry->unregisterWorld(mWorld);
			mRegistry.reset();
			mReference.reset();
			if (mPolicy.afterDetach) mPolicy.afterDetach();
			mWorld.modify();
		}

		void detachAndClearAssignments()
		{
			mWorld.invalidateSimulationSnapshot();
			if (!mReference) return;
			if (mPolicy.requirePausedForDetach) mPolicy.requirePausedForDetach();
			CommitHook commit;
			if (mPolicy.clearAssignmentsBeforeDetach)
				commit = mPolicy.clearAssignmentsBeforeDetach();
			if (mRegistry) mRegistry->unregisterWorld(mWorld);
			if (commit) commit();
			mRegistry.reset();
			mReference.reset();
			if (mPolicy.afterDetach) mPolicy.afterDetach();
			mWorld.modify();
		}

		// Open-time resolution of a persisted reference: the loaded registry
		// must carry the expected UUID before the family's reconciliation
		// body runs.
		void resolve(std::shared_ptr<Registry> registry)
		{
			mWorld.invalidateSimulationSnapshot();
			if (!mReference)
				throw std::invalid_argument("The World has no " + mPolicy.label
					+ " reference to resolve");
			if (!registry)
				throw std::invalid_argument("Cannot resolve a null "
					+ mPolicy.label);
			if (registry->getUuid() != mReference->expectedUuid)
				throw std::runtime_error(mPolicy.label
					+ " UUID mismatch: World expects "
					+ mReference->expectedUuid + ", file contains "
					+ registry->getUuid());
			mPolicy.resolveBody(std::move(registry), mRegistry);
		}

		// Save As: move an intact World into an equivalent, independent
		// registry Resource. Assignments remain unchanged while the registry
		// UUID and Resource name are replaced atomically.
		void replaceWithIndependentCopy(std::string resourceName,
			std::shared_ptr<Registry> registry)
		{
			mWorld.invalidateSimulationSnapshot();
			if (mPolicy.requirePausedForReplaceCopy)
				mPolicy.requirePausedForReplaceCopy();
			requireResourceName(resourceName);
			auto const commit = mPolicy.preflightReplaceCopy(resourceName,
				registry);
			commitSwap(std::move(resourceName), std::move(registry), commit,
				mPolicy.afterReplaceCopy, false);
		}

		// A package load failure is dependency state, not malformed World
		// data: the persisted reference and unresolved assignments remain
		// available for repair.
		void markUnavailable(std::string diagnostic)
		{
			mWorld.invalidateSimulationSnapshot();
			if (!mReference) return;
			if (mPolicy.markUnavailable)
				mPolicy.markUnavailable(*mReference, std::move(diagnostic));
		}

	private:
		// An external registry/catalogue reference is an application Resource
		// name (ADR 0010), not a file path: a non-empty name with no
		// directory part.
		void requireResourceName(std::string const& resourceName) const
		{
			std::filesystem::path const path(resourceName);
			if (resourceName.empty() || path.is_absolute() || path.has_parent_path()
				|| path.filename().string() != resourceName)
				throw std::invalid_argument(std::string("A ") + mPolicy.label
					+ " reference must be a Resource name");
		}

		void requireValidRegistry(std::string const& message,
			std::shared_ptr<Registry> const& registry) const
		{
			if (!registry || !Registry::uuidIsValid(registry->getUuid()))
				throw std::invalid_argument(message);
		}

		bool identicalAttached(std::string const& resourceName,
			std::shared_ptr<Registry> const& registry) const
		{
			return mReference
				&& mReference->resourceName == resourceName
				&& mReference->expectedUuid == registry->getUuid()
				&& mRegistry == registry;
		}

		bool sameNamespace(Registry const& registry) const
		{
			return mReference && mReference->expectedUuid == registry.getUuid();
		}

		// The one commit path: register the replacement, run the family's
		// swap closure between registration and unregistration, publish the
		// new reference/pointer pair as one transition, run the family's
		// after hook, and dirty the World.
		void commitSwap(std::string resourceName,
			std::shared_ptr<Registry> registry,
			CommitHook const& between,
			std::function<void()> const& after,
			bool modifyOnlyIfReferenceChanged)
		{
			Reference replacement{ std::move(resourceName),
				registry->getUuid() };
			bool const referenceChanges = !mReference
				|| mReference->resourceName != replacement.resourceName
				|| mReference->expectedUuid != replacement.expectedUuid;
			bool const registryChanges = mRegistry != registry;
			if (registryChanges) registry->registerWorld(mWorld);
			if (between) between();
			if (registryChanges && mRegistry)
				mRegistry->unregisterWorld(mWorld);
			mReference = std::move(replacement);
			mRegistry = std::move(registry);
			if (after) after();
			if (!modifyOnlyIfReferenceChanged || referenceChanges)
				mWorld.modify();
		}

		World& mWorld;
		Policy mPolicy;
		// References to the World-owned state (ADR 0001): the binding never
		// takes ownership, so serialization and replay keep writing the same
		// fields the binding reads.
		std::optional<Reference>& mReference;
		std::shared_ptr<Registry>& mRegistry;
	};
}
