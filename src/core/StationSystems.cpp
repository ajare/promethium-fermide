#include "StationSystemsRuntime.h"
#include "core/World.h"
#include "core/SerializationException.h"
#include "BundledStationSystemsSource.h"
#include "ScriptSandbox.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>

namespace core
{
	namespace
	{
		StationSystemsSourceLoader sourceLoader;
		constexpr size_t EventLimit = 256;
		constexpr size_t DiagnosticByteLimit = 2048;
		// Borrow the Lua error only long enough to copy a bounded string. Never
		// coerce arbitrary error objects outside a protected allocation boundary.
		std::string errorMessage(lua_State* state, char const* fallback)
		{
			if (lua_type(state, -1) != LUA_TSTRING) return fallback;
			size_t length = 0;
			auto const* text = lua_tolstring(state, -1, &length);
			return std::string(text, std::min(length, DiagnosticByteLimit));
		}
		struct ScriptObject
		{
			script::ScratchBudget budget{512 * 1024, 100'000};
			script::ModuleLoader loader;
			std::unique_ptr<lua_State, script::StateCloser> state;
			int reference{LUA_NOREF};
			ScriptObject() : state(lua_newstate(script::budgetedAllocate, &budget))
			{
				if (!state) throw SerializationException("Cannot allocate Station systems sandbox");
				lua_atpanic(state.get(), script::luaPanic);
				std::string diagnostic;
				if (script::runScratchSetup(state.get(), &loader, nullptr, diagnostic) != LUA_OK)
					throw SerializationException(diagnostic);
				lua_settop(state.get(), 0);
			}
		};
		struct Construction
		{
			ScriptObject* object;
			std::string const* source;
			char const* name;
			std::vector<ResourceStoreSettings> const* stores;
			std::vector<ResourceTransferSettings> const* transfers;
			int operator()(lua_State* state) const
			{
				script::pushPrivateEnvironment(state);
				if (luaL_loadbufferx(state, source->data(), source->size(), "@station-systems/lifecycle.lua", "t") != LUA_OK)
					return lua_error(state);
				lua_pushvalue(state, -2); lua_setupvalue(state, -2, 1);
				lua_call(state, 0, 1);
				lua_pushstring(state, name);
				// Authored store descriptors cross the sandbox boundary as plain
				// immutable values; the Lua model initialises its own copy.
				lua_createtable(state, static_cast<int>(stores->size()), 0);
				for (size_t index = 0; index < stores->size(); ++index)
				{
					auto const& store = (*stores)[index];
					lua_createtable(state, 0, 6);
					lua_pushstring(state, store.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushstring(state, store.unit.c_str()); lua_setfield(state, -2, "unit");
					lua_pushnumber(state, store.capacity); lua_setfield(state, -2, "capacity");
					lua_pushnumber(state, store.startingQualified); lua_setfield(state, -2, "qualified");
					lua_pushnumber(state, store.startingUnqualified); lua_setfield(state, -2, "unqualified");
					lua_pushnumber(state, store.protectedReserve); lua_setfield(state, -2, "protectedReserve");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				// Authored transfer rules cross the same boundary as plain
				// immutable values; each sandbox mirrors and audits them itself.
				lua_createtable(state, static_cast<int>(transfers->size()), 0);
				for (size_t index = 0; index < transfers->size(); ++index)
				{
					auto const& transfer = (*transfers)[index];
					lua_createtable(state, 0, 6);
					lua_pushstring(state, transfer.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushstring(state, transfer.sourceId.c_str()); lua_setfield(state, -2, "source");
					lua_pushstring(state, transfer.destinationId.c_str()); lua_setfield(state, -2, "destination");
					lua_pushstring(state, transfer.unit.c_str()); lua_setfield(state, -2, "unit");
					lua_pushnumber(state, transfer.ratePerSecond); lua_setfield(state, -2, "rate");
					lua_pushinteger(state, transfer.priority); lua_setfield(state, -2, "priority");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				lua_call(state, 3, 1);
				if (!lua_istable(state, -1)) return luaL_error(state, "Station factory must return an object");
				object->reference = luaL_ref(state, LUA_REGISTRYINDEX);
				return 0;
			}
		};
		struct Step
		{
			ScriptObject* object;
			uint64_t tick;
			float seconds;
			uint64_t completed{};
			bool invalidOutput{false};
			int operator()(lua_State* state)
			{
				lua_rawgeti(state, LUA_REGISTRYINDEX, object->reference);
				lua_getfield(state, -1, "advance"); lua_pushvalue(state, -2);
				lua_pushinteger(state, static_cast<lua_Integer>(tick));
				lua_pushnumber(state, seconds); lua_call(state, 3, 1);
				if (!lua_isinteger(state, -1) || lua_tointeger(state, -1) < 0)
				{
					invalidOutput = true;
					if (lua_type(state, -1) == LUA_TNUMBER && !std::isfinite(lua_tonumber(state, -1)))
						return luaL_error(state, "Station lifecycle returned non-finite completed ticks");
					return luaL_error(state, "Station lifecycle returned invalid completed ticks");
				}
				completed = static_cast<uint64_t>(lua_tointeger(state, -1));
				return 0;
			}
		};
		std::string declaredUnitsList()
		{
			std::string list;
			for (auto const* unit : StationStoreUnits)
			{
				if (!list.empty()) list += ", ";
				list += unit;
			}
			return list;
		}
	}
	bool validateStationSystemsSettings(StationSystemsSettings const& settings, std::string& diagnostic)
	{
		auto refuse = [&](std::string text) { diagnostic = std::move(text); return false; };
		if (!std::isfinite(settings.observationIntervalSeconds)
			|| settings.observationIntervalSeconds < 0.1f || settings.observationIntervalSeconds > 60.0f)
			return refuse("Station observation interval must be finite, from 0.1 to 60 seconds");
		if (settings.stores.size() > MaxStationResourceStores)
			return refuse("Station resource stores are limited to "
				+ std::to_string(MaxStationResourceStores) + " entries");
		std::set<std::string> ids;
		for (auto const& store : settings.stores)
		{
			auto const where = "Station resource store '" + store.id + "'";
			if (store.id.empty()) return refuse("Station resource store id must not be empty");
			if (store.id.size() > MaxStationStoreIdLength)
				return refuse("Station resource store id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!ids.insert(store.id).second) return refuse(where + " id is duplicated");
			if (std::find(StationStoreUnits.begin(), StationStoreUnits.end(), store.unit)
				== StationStoreUnits.end())
				return refuse(where + " has unknown unit '" + store.unit
					+ "'; declared units are: " + declaredUnitsList());
			if (!std::isfinite(store.capacity) || store.capacity <= 0.0)
				return refuse(where + " capacity must be finite and positive");
			if (!std::isfinite(store.startingQualified) || !std::isfinite(store.startingUnqualified))
				return refuse(where + " starting inventory must be finite");
			if (store.startingQualified < 0.0 || store.startingUnqualified < 0.0)
				return refuse(where + " starting inventory must not be negative");
			if (store.startingQualified + store.startingUnqualified > store.capacity)
				return refuse(where + " starting inventory exceeds capacity");
			if (!std::isfinite(store.protectedReserve) || store.protectedReserve < 0.0)
				return refuse(where + " protected reserve must be finite and non-negative");
			if (store.protectedReserve > store.startingQualified)
				return refuse(where + " protected reserve exceeds starting usable inventory");
		}
		if (settings.transfers.size() > MaxStationResourceTransfers)
			return refuse("Station resource transfers are limited to "
				+ std::to_string(MaxStationResourceTransfers) + " entries");
		std::set<std::string> transferIds;
		for (auto const& transfer : settings.transfers)
		{
			auto const where = "Station resource transfer '" + transfer.id + "'";
			if (transfer.id.empty()) return refuse("Station resource transfer id must not be empty");
			if (transfer.id.size() > MaxStationStoreIdLength)
				return refuse("Station resource transfer id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!transferIds.insert(transfer.id).second) return refuse(where + " id is duplicated");
			if (transfer.sourceId == transfer.destinationId)
				return refuse(where + " source and destination must differ");
			auto const findStore = [&](std::string const& id) -> ResourceStoreSettings const*
			{
				auto const it = std::find_if(settings.stores.begin(), settings.stores.end(),
					[&](ResourceStoreSettings const& store) { return store.id == id; });
				return it == settings.stores.end() ? nullptr : &*it;
			};
			auto const* source = findStore(transfer.sourceId);
			auto const* destination = findStore(transfer.destinationId);
			if (!source) return refuse(where + " references unknown source store '" + transfer.sourceId + "'");
			if (!destination)
				return refuse(where + " references unknown destination store '" + transfer.destinationId + "'");
			// Same-unit endpoints only: cross-unit movement is conversion, which
			// later slices introduce with explicit composition accounting.
			if (transfer.unit != source->unit || transfer.unit != destination->unit)
				return refuse(where + " unit '" + transfer.unit
					+ "' does not match both stores ('" + source->unit + "' -> '" + destination->unit + "')");
			if (!std::isfinite(transfer.ratePerSecond) || transfer.ratePerSecond <= 0.0)
				return refuse(where + " rate must be finite and positive");
		}
		diagnostic.clear(); return true;
	}
	void setStationSystemsSourceLoader(StationSystemsSourceLoader loader) { sourceLoader = std::move(loader); }
	std::string bundledStationSystemsSource() { return std::string(kStationSystemsLua); }
	struct StationSystemsRuntime::Impl
	{
		// Current inventory is runtime state only: it is initialised from the
		// authored starting quantities when the runtime is constructed and is
		// never written back into the authored settings or the document.
		struct StoreState
		{
			ResourceStoreSettings authored;
			double currentQualified{0.0};
			double currentUnqualified{0.0};
			double lastTickIn{0.0};
			double lastTickOut{0.0};
		};
		// The authoritative ledger of one authored transfer. Results are
		// staged per tick and committed atomically; unmet material never moves
		// and is never deleted.
		struct TransferState
		{
			ResourceTransferSettings authored;
			size_t sourceIndex{0};
			size_t destinationIndex{0};
			double lastRequested{0.0};
			double lastCompleted{0.0};
			double lastUnmet{0.0};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		StationSystemsSettings settings;
		std::vector<StoreState> stores;
		std::vector<TransferState> transfers;
		// Competing transfers are allocated in priority order (higher first),
		// ties resolved by declaration order: a declared deterministic order.
		std::vector<size_t> transferOrder;
		// Per-unit totals at run start, the conservation boundary (#390).
		std::map<std::string, double> startingTotals;
		std::array<std::unique_ptr<ScriptObject>, 4> objects;
		StationSystemsSnapshot snapshot;
		std::vector<StationSystemsEvent> events;
		std::vector<StationSystemsDiagnostic> diagnostics;

		// Apply every authored transfer to a staged copy of the inventory in
		// the declared deterministic order, audit the staged result against
		// finite limits and the per-unit conservation boundary, and only then
		// commit. A violation never clamps material into existence or out of
		// it: it fails the whole tick through the integrated error path.
		bool applyTransfers(uint64_t tick, float seconds, std::string& violation)
		{
			std::vector<StationTransferStatus> previousStatus;
			previousStatus.reserve(transfers.size());
			for (auto const& transfer : transfers) previousStatus.push_back(transfer.status);
			std::vector<double> qualified, unqualified;
			qualified.reserve(stores.size());
			unqualified.reserve(stores.size());
			for (auto const& store : stores)
			{
				qualified.push_back(store.currentQualified);
				unqualified.push_back(store.currentUnqualified);
			}
			std::vector<double> flowIn(stores.size(), 0.0), flowOut(stores.size(), 0.0);
			std::vector<std::string> stallReason(transfers.size());
			for (auto const index : transferOrder)
			{
				auto& transfer = transfers[index];
				auto const& source = stores[transfer.sourceIndex];
				auto const& destination = stores[transfer.destinationIndex];
				auto const requested = transfer.authored.ratePerSecond * static_cast<double>(seconds);
				// Only qualified material above the protected reserve may move,
				// and only into the destination's remaining total capacity.
				auto const drawable = std::max(0.0, qualified[transfer.sourceIndex] - source.authored.protectedReserve);
				auto const headroom = destination.authored.capacity
					- (qualified[transfer.destinationIndex] + unqualified[transfer.destinationIndex]);
				auto const completed = std::min({requested, drawable, headroom});
				qualified[transfer.sourceIndex] -= completed;
				qualified[transfer.destinationIndex] += completed;
				flowOut[transfer.sourceIndex] += completed;
				flowIn[transfer.destinationIndex] += completed;
				transfer.lastRequested = requested;
				transfer.lastCompleted = completed;
				transfer.lastUnmet = requested - completed;
				transfer.status = completed <= 0.0 ? StationTransferStatus::Rejected
					: (completed >= requested ? StationTransferStatus::Full : StationTransferStatus::Partial);
				if (transfer.status != StationTransferStatus::Full)
					stallReason[index] = drawable <= 0.0
						? (source.authored.protectedReserve > 0.0
							? "source usable inventory is at its protected reserve"
							: "source usable inventory is empty")
						: (headroom <= 0.0 ? "destination store is at capacity" : "limited supply");
			}
			std::map<std::string, double> currentTotals;
			for (size_t index = 0; index < stores.size(); ++index)
			{
				if (!std::isfinite(qualified[index]) || !std::isfinite(unqualified[index]))
					return violation = "transfer left non-finite inventory in store '" + stores[index].authored.id + "'", false;
				if (qualified[index] < 0.0 || unqualified[index] < 0.0)
					return violation = "transfer drove inventory negative in store '" + stores[index].authored.id + "'", false;
				if (qualified[index] + unqualified[index] > stores[index].authored.capacity)
					return violation = "transfer exceeded capacity of store '" + stores[index].authored.id + "'", false;
				currentTotals[stores[index].authored.unit] += qualified[index] + unqualified[index];
			}
			for (auto const& [unit, starting] : startingTotals)
			{
				auto const residual = currentTotals[unit] - starting;
				if (!std::isfinite(residual)
					|| std::abs(residual) > StationBalanceTolerance * std::max(1.0, std::abs(starting)))
					return violation = "conservation residual " + std::to_string(residual)
						+ " for unit '" + unit + "' exceeds tolerance " + std::to_string(StationBalanceTolerance), false;
			}
			for (size_t index = 0; index < stores.size(); ++index)
			{
				stores[index].currentQualified = qualified[index];
				stores[index].currentUnqualified = unqualified[index];
				stores[index].lastTickIn = flowIn[index];
				stores[index].lastTickOut = flowOut[index];
			}
			// Rejections and partial completions are visible: announce each
			// status transition (after the commit) so the event log records
			// where material stalled without flooding every tick.
			for (auto const index : transferOrder)
			{
				auto const& transfer = transfers[index];
				if (transfer.status == previousStatus[index]) continue;
				if (transfer.status == StationTransferStatus::Full)
				{
					events.push_back({tick, "Transfer '" + transfer.authored.id + "' supplying fully"});
					continue;
				}
				auto const& reason = stallReason[index];
				auto const message = transfer.status == StationTransferStatus::Rejected
					? std::string("Transfer '") + transfer.authored.id + "' rejected: " + reason
					: std::string("Transfer '") + transfer.authored.id + "' partial: " + reason;
				events.push_back({tick, message});
			}
			return true;
		}
	};
	StationSystemsRuntime::StationSystemsRuntime(StationSystemsSettings settings) : mImpl(std::make_unique<Impl>())
	{
		std::string diagnostic;
		if (!validateStationSystemsSettings(settings, diagnostic)) throw SerializationException(diagnostic);
		mImpl->settings = settings; mImpl->snapshot.enabled = settings.enabled;
		for (auto const& store : settings.stores)
		{
			mImpl->stores.push_back({store, store.startingQualified, store.startingUnqualified, 0.0, 0.0});
			mImpl->startingTotals[store.unit] += store.startingQualified + store.startingUnqualified;
		}
		std::map<std::string, size_t> storeIndex;
		for (size_t index = 0; index < mImpl->stores.size(); ++index)
			storeIndex[mImpl->stores[index].authored.id] = index;
		for (auto const& transfer : settings.transfers)
			mImpl->transfers.push_back({transfer, storeIndex[transfer.sourceId], storeIndex[transfer.destinationId]});
		mImpl->transferOrder.resize(mImpl->transfers.size());
		std::iota(mImpl->transferOrder.begin(), mImpl->transferOrder.end(), size_t{0});
		std::stable_sort(mImpl->transferOrder.begin(), mImpl->transferOrder.end(),
			[&](size_t left, size_t right)
			{ return mImpl->transfers[left].authored.priority > mImpl->transfers[right].authored.priority; });
		if (!settings.enabled) return;
		auto const source = sourceLoader ? sourceLoader() : bundledStationSystemsSource();
		if (source.empty() || source.size() > 64 * 1024) throw SerializationException("Invalid Station systems source size");
		for (size_t i = 0; i < mImpl->objects.size(); ++i)
		{
			auto object = std::make_unique<ScriptObject>();
			Construction construction{object.get(), &source, StationSystemNames[i], &settings.stores, &settings.transfers};
			script::beginInstructionBudget(object->state.get(), object->budget);
			auto status = script::protectedLuaOperation(object->state.get(), construction, 0, 0);
			script::endInstructionBudget(object->state.get());
			if (status != LUA_OK || script::budgetExhausted(object->budget))
			{
				throw SerializationException(std::string(StationSystemNames[i]) + ": "
					+ (script::budgetExhausted(object->budget) ? std::string(script::budgetDiagnostic(object->budget))
						: errorMessage(object->state.get(), "Station constructor failed")));
			}
			lua_settop(object->state.get(), 0);
			mImpl->objects[i] = std::move(object);
		}
		mImpl->events.push_back({0, "Station systems initialized with "
			+ std::to_string(settings.stores.size()) + " resource store(s) and "
			+ std::to_string(settings.transfers.size()) + " transfer(s)"});
	}
	StationSystemsRuntime::~StationSystemsRuntime() = default;
	bool StationSystemsRuntime::advance(uint64_t tick, float seconds)
	{
		if (!mImpl->settings.enabled) return true;
		if (!mImpl->snapshot.healthy) return false;
		auto next = mImpl->snapshot;
		for (size_t i = 0; i < mImpl->objects.size(); ++i)
		{
			auto& object = *mImpl->objects[i];
			Step step{&object, tick, seconds};
			try
			{
				script::beginInstructionBudget(object.state.get(), object.budget);
				auto status = script::protectedLuaOperation(object.state.get(), step, 0, 0);
				script::endInstructionBudget(object.state.get());
				if (status != LUA_OK || script::budgetExhausted(object.budget))
				{
					throw std::runtime_error(script::budgetExhausted(object.budget)
						? std::string(script::budgetDiagnostic(object.budget)) : errorMessage(object.state.get(), "Station step failed"));
				}
				if (step.completed != next.completedTicks[i] + 1)
				{
					step.invalidOutput = true;
					throw std::runtime_error("Station lifecycle tick accounting mismatch");
				}
				next.completedTicks[i] = step.completed;
				lua_settop(object.state.get(), 0);
			}
			catch (std::exception const& error)
			{
				script::endInstructionBudget(object.state.get());
				mImpl->snapshot.healthy = false;
				mImpl->diagnostics.push_back({tick, StationSystemNames[i],
					script::failureKind(object.budget, step.invalidOutput
						? ScriptExecutionFailure::ConversionError : ScriptExecutionFailure::LuaError),
					std::string(error.what(), std::min(std::char_traits<char>::length(error.what()), DiagnosticByteLimit))});
				// Retire all mutated objects. No failed step can be retried, and no
				// partial result escapes into the public committed snapshot.
				for (auto& owned : mImpl->objects) owned.reset();
				return false;
			}
		}
		// The authoritative transfer ledger runs once every sandbox step has
		// succeeded: its staged inventory and events commit atomically, so a
		// conservation or limit violation never publishes a partial result
		// and never clamps material. It fails the whole tick through the
		// integrated error path instead.
		if (!mImpl->transferOrder.empty())
		{
			std::string violation;
			if (!mImpl->applyTransfers(tick, seconds, violation))
			{
				mImpl->snapshot.healthy = false;
				mImpl->diagnostics.push_back({tick, "Resource transfer",
					ScriptExecutionFailure::ConversionError, violation});
				for (auto& owned : mImpl->objects) owned.reset();
				return false;
			}
		}
		next.worldTick = tick;
		next.elapsedSeconds = static_cast<double>(tick) * World::getFixedTimestep();
		mImpl->snapshot = next;
		auto interval = static_cast<uint64_t>(std::ceil(mImpl->settings.observationIntervalSeconds / World::getFixedTimestep()));
		if (tick % interval == 0)
		{
			if (mImpl->events.size() == EventLimit) mImpl->events.erase(mImpl->events.begin());
			mImpl->events.push_back({tick, "All four Station systems completed the World tick"});
		}
		return true;
	}
	StationSystemsSnapshot StationSystemsRuntime::snapshot() const
	{
		auto result = mImpl->snapshot;
		std::map<std::string, double> currentTotals;
		for (auto const& store : mImpl->stores)
		{
			result.stores.push_back({store.authored.id, store.authored.unit,
				store.authored.capacity, store.authored.startingQualified,
				store.authored.startingUnqualified, store.authored.protectedReserve,
				store.currentQualified, store.currentUnqualified,
				store.lastTickIn, store.lastTickOut});
			currentTotals[store.authored.unit] += store.currentQualified + store.currentUnqualified;
		}
		for (auto const index : mImpl->transferOrder)
		{
			auto const& transfer = mImpl->transfers[index];
			result.transfers.push_back({transfer.authored.id, transfer.authored.sourceId,
				transfer.authored.destinationId, transfer.authored.unit,
				transfer.authored.ratePerSecond, transfer.authored.priority,
				transfer.lastRequested, transfer.lastCompleted, transfer.lastUnmet, transfer.status});
		}
		for (auto const& [unit, starting] : mImpl->startingTotals)
			result.balances.push_back({unit, starting, currentTotals[unit], currentTotals[unit] - starting});
		result.balanceTolerance = StationBalanceTolerance;
		return result;
	}
	std::vector<StationSystemsEvent> const& StationSystemsRuntime::events() const { return mImpl->events; }
	std::vector<StationSystemsDiagnostic> const& StationSystemsRuntime::diagnostics() const { return mImpl->diagnostics; }
}
