#include "StationSystemsImpl.h"
namespace core
{
	StationForecastSnapshot StationSystemsRuntime::Impl::computeForecast(double asOfSeconds) const
	{
		StationForecastSnapshot result;
		result.horizonSeconds = StationForecastHorizonSeconds;
		result.asOfSeconds = asOfSeconds;
		// Nothing is modelled before the forecast instant; a disabled
		// Station never completes any range (#604).
		result.completeThroughSeconds = asOfSeconds;
		result.scenarioLabel =
			"declared-profile scenario: authored transfer, crew and equipment demand profiles continue unchanged; "
			"a manually commanded generator's fuel draw is projected as deterministic demand; "
			"scheduled deliveries land as authored with capacity-capped acceptance; ejections remove only their declared "
			"projected partitions (qualified by default, held-only or qualified-first then held when declared); biology and process-chain recovery are never counted as guaranteed supply";
		result.uncertaintyLabel =
			"provisional biology: crop, algae and microbial recovery pathways are tunable model assumptions, not "
			"validated biology, and are flagged and excluded from the guaranteed projection; process-chain production "
			"(treatment, electrolysis, conversion, capture) is not projected; consuming pathways other than manually "
			"commanded generator fuel are not projected either - stores they drain carry processConsumptionPossible or "
			"biologyConsumptionPossible, and a Sustained basis beside those flags does not rule out an earlier live "
			"shortfall from the omitted draw";
		if (!settings.enabled)
		{
			result.unavailableReason = "Station systems are disabled.";
			return result;
		}
		// This declared-profile projection is not the authoritative transport
		// allocator. Do not promise inventory/resupply outcomes that ignore
		// authored caps or losses, or duplicate the allocator here. Use the
		// same relationship classification as the live connection ledger:
		// carrier styling and non-physical annotations do not change eligibility.
		StationConnectionProperties const defaults;
		for (auto const& link : connectionGraph.links)
		{
			if (station_connections::automaticConnectionType(connectionGraph, link)
				== StationPhysicalConnection::NonPhysical) continue;
			auto const properties = station_connections::connectionProperties(settings, link);
			if (properties.maximumThroughput != defaults.maximumThroughput
				|| properties.efficiency != defaults.efficiency)
			{
				result.unavailableReason = "Physical connection throughput or efficiency differs from defaults; "
					"the declared-profile projection does not model connection caps and losses.";
				return result;
			}
		}
		for (auto const& crew : settings.crews)
			if (crew.greywaterKgPerCrewDay > 0.0)
			{
				result.unavailableReason = "Crew greywater returns and their quality-dependent downstream use are not modelled by the declared-profile forecast.";
				return result;
			}
		result.available = true;
		// Exclude the complete material-connected component, not just the
		// two endpoints: downstream Crew/transfer projections depend on it.
		std::vector<bool> scheduledAffected(connectionGraph.nodes.size(), false);
		for (size_t n = 0; n < connectionGraph.nodes.size(); ++n)
			if (connectionGraph.nodes[n].type == "Transfer")
				for (auto const& transfer : settings.transfers)
					if (transfer.id == connectionGraph.nodes[n].id && transfer.mode == ResourceTransferMode::Scheduled)
						scheduledAffected[n] = true;
		for (bool changed = true; changed; )
		{
			changed = false;
			for (auto const& link : connectionGraph.links)
			{
				if (link.kind != station_connections::LinkKind::Material && link.designKey.ownerType != "Transfer") continue;
				if (scheduledAffected[link.from] == scheduledAffected[link.to]) continue;
				scheduledAffected[link.from] = scheduledAffected[link.to] = true; changed = true;
			}
		}
		std::set<std::string> scheduledStores;
		for (size_t n = 0; n < connectionGraph.nodes.size(); ++n)
			if (scheduledAffected[n] && connectionGraph.nodes[n].type == "Store") scheduledStores.insert(connectionGraph.nodes[n].id);
		auto const horizonEnd = asOfSeconds + StationForecastHorizonSeconds;
		// The projection runs on a local copy of the store inventory:
		// the live committed state is read, never written.
		struct Projected
		{
			double qualified{0.0};
			double unqualified{0.0};
			double inflowMoved{0.0};
			double outflowMoved{0.0};
			double acceptedQualified{0.0};
			double acceptedUnqualified{0.0};
			double unmetOutflow{0.0};
			bool depleted{false};
			double depletionOnset{0.0};
			std::string limiterId;
			std::string limiterKind;
		};
		std::vector<Projected> projected(stores.size());
		for (size_t index = 0; index < stores.size(); ++index)
		{
			projected[index].qualified = stores[index].currentQualified;
			projected[index].unqualified = stores[index].currentUnqualified;
		}
		auto const drawableOf = [&](size_t index)
		{
			return std::max(0.0, projected[index].qualified
				- stores[index].authored.protectedReserve);
		};
		auto const headroomOf = [&](size_t index)
		{
			return std::max(0.0, stores[index].authored.capacity
				- (projected[index].qualified + projected[index].unqualified));
		};
		// A relative epsilon on the dry/full boundaries: a residual
		// below the balance tolerance scaled by the store's own
		// physical magnitude reads as empty/full, so floating-point
		// ulps left by an exact crossing cannot spin the loop through
		// tiny segments toward the cap.
		auto const isDry = [&](size_t index)
		{
			return drawableOf(index) <= StationBalanceTolerance
				* std::max(1.0, projected[index].qualified
					+ projected[index].unqualified);
		};
		auto const isFull = [&](size_t index)
		{
			return headroomOf(index) <= StationBalanceTolerance
				* std::max(1.0, projected[index].qualified
					+ projected[index].unqualified);
		};
		// Collect the authored event times inside the horizon.
		// Deliveries and ejections share the scenario event ordering
		// (#392): occurrence time, then service priority descending,
		// then declaration order, with arrivals ahead of ejections at
		// equal time and priority - the same rule the live ledger
		// applies. Profile segment starts become breakpoints at which
		// the demand levels step.
		//
		// #604: each delivery, each ejection and each profile segment
		// is one stream of occurrences, and the budget is filled by
		// merging the streams at their chronological frontier: the
		// earliest next occurrence across all streams is taken first,
		// so exhausting one declaration in turn can no longer fill the
		// budget before a later-declared schedule's early events are
		// ever considered. When the budget fills, the frontier time is
		// the earliest event the projection cannot model; the run
		// stops there and reports it as the complete-through bound
		// rather than projecting a hole.
		struct TimedEvent
		{
			StationScenarioEventKey key;
			bool isDelivery{true};
			size_t configIndex{0};
		};
		std::vector<TimedEvent> events;
		std::vector<double> breakpoints;
		bool truncated = false;
		// The earliest authored occurrence the budget could not
		// include; horizonEnd until collection actually overflows.
		double firstOmittedSeconds = horizonEnd;
		struct OccurrenceStream
		{
			double first{0.0};
			double recurrence{0.0}; // <= 0: a single one-off occurrence
			long long occurrence{0}; // index of `next` in the stream
			double next{0.0};
			bool isBreakpoint{false};
			bool isDelivery{true};
			size_t configIndex{0};
			int priority{0};
			size_t declaration{0};
			size_t streamId{0};
		};
		// Min-heap on the frontier time; the stream id breaks ties so
		// the merge order is deterministic whatever the heap does.
		auto const frontierLess = [](OccurrenceStream const& left,
			OccurrenceStream const& right)
		{
			if (left.next != right.next) return left.next > right.next;
			return left.streamId > right.streamId;
		};
		std::priority_queue<OccurrenceStream, std::vector<OccurrenceStream>,
			decltype(frontierLess)> frontier(frontierLess);
		auto const advanceStream = [&](OccurrenceStream& stream)
		{
			if (stream.recurrence <= 0.0) return false; // one-off: done
			++stream.occurrence;
			stream.next = stream.first
				+ static_cast<double>(stream.occurrence) * stream.recurrence;
			if (stream.next > horizonEnd + StationScheduleTolerance)
				return false;
			frontier.push(stream);
			return true;
		};
		auto const addStream = [&](double first, double recurrence,
			bool isBreakpoint, bool isDelivery, size_t configIndex,
			int priority, size_t declaration)
		{
			OccurrenceStream stream;
			stream.first = first;
			stream.recurrence = recurrence;
			stream.isBreakpoint = isBreakpoint;
			stream.isDelivery = isDelivery;
			stream.configIndex = configIndex;
			stream.priority = priority;
			stream.declaration = declaration;
			stream.streamId = frontier.size();
			if (recurrence <= 0.0)
			{
				// A one-off outside the projected window models
				// nothing: it never enters the merge and never
				// spends budget.
				if (first <= asOfSeconds + StationScheduleTolerance
					|| first > horizonEnd + StationScheduleTolerance)
					return;
				stream.occurrence = 0;
				stream.next = first;
			}
			else
			{
				long long index = 0;
				if (first <= asOfSeconds + StationScheduleTolerance)
					index = static_cast<long long>(std::ceil(
						(asOfSeconds + StationScheduleTolerance - first)
							/ recurrence));
				stream.occurrence = index;
				stream.next = first
					+ static_cast<double>(index) * recurrence;
				if (stream.next > horizonEnd + StationScheduleTolerance)
					return;
			}
			frontier.push(stream);
		};
		for (size_t index = 0; index < deliveries.size(); ++index)
			addStream(deliveries[index].authored.firstOccurrenceSeconds,
				deliveries[index].authored.recurrenceSeconds, false, true,
				index, deliveries[index].authored.priority,
				deliveries[index].declarationIndex);
		for (size_t index = 0; index < wasteEjections.size(); ++index)
			addStream(wasteEjections[index].authored.firstOccurrenceSeconds,
				wasteEjections[index].authored.recurrenceSeconds, false,
				false, index, wasteEjections[index].authored.priority,
				wasteEjections[index].declarationIndex);
		auto const addProfileStreams = [&](ProfileSettings const& profile)
		{
			if (profile.periodSeconds <= 0.0) return;
			for (auto const& segment : profile.segments)
				addStream(segment.startSeconds, profile.periodSeconds,
					true, true, 0, 0, 0);
		};
		for (auto const& crew : crews)
			addProfileStreams(crew.authored.activityProfile);
		for (auto const& load : loads)
			addProfileStreams(load.authored.operatingProfile);
		while (!frontier.empty())
		{
			auto stream = frontier.top();
			if (stream.next > horizonEnd + StationScheduleTolerance) break;
			// A breakpoint already collected at this exact instant
			// carries no new information: consume the duplicate
			// without spending budget. Streams leave the frontier in
			// non-decreasing time order, so the last breakpoint
			// collected is the only possible duplicate.
			if (stream.isBreakpoint && !breakpoints.empty()
				&& breakpoints.back() == stream.next)
			{
				frontier.pop();
				advanceStream(stream);
				continue;
			}
			if (events.size() + breakpoints.size()
				>= StationForecastMaxSegments)
			{
				// The budget is spent and this frontier event is real:
				// the projection is complete only up to here.
				truncated = true;
				firstOmittedSeconds = stream.next;
				break;
			}
			frontier.pop();
			if (stream.isBreakpoint)
				breakpoints.push_back(stream.next);
			else
				events.push_back({{stream.next, stream.priority,
					stream.declaration}, stream.isDelivery,
					stream.configIndex});
			advanceStream(stream);
		}
		std::sort(events.begin(), events.end(),
			[](TimedEvent const& left, TimedEvent const& right)
			{
				if (left.key < right.key) return true;
				if (right.key < left.key) return false;
				// Equal time, priority and declaration slot: arrivals run
				// before ejections, matching the live scenario order.
				return left.isDelivery && !right.isDelivery;
			});
		std::sort(breakpoints.begin(), breakpoints.end());
		breakpoints.erase(std::unique(breakpoints.begin(), breakpoints.end()),
			breakpoints.end());
		// The per-segment rate solve: between events every declared
		// rate is constant, so each store's projected trajectory is
		// linear and crossings are exact.
		std::vector<double> net(stores.size(), 0.0);
		std::vector<double> maxOutflow(stores.size(), 0.0);
		std::vector<std::string> limiterId(stores.size());
		std::vector<std::string> limiterKind(stores.size());
		struct BlockedFlow
		{
			size_t storeIndex{0};
			double rate{0.0};
		};
		std::vector<BlockedFlow> blocked;
		// #594: under Automatic dispatch a generator's draw follows the
		// unmodelled net load rather than an authored setpoint, so it
		// is not projected and the fuel store is flagged as carrying
		// unprojected consumption instead.
		auto const forecastDispatchAutomatic
			= settings.dispatchMode == StationDispatchMode::Automatic;
		// #593/#605: per-store outflow demand across every projected
		// pathway (transfer, equipment, crew and commanded generator
		// draw), the total blocked outflow demand, and the through-flow
		// coverage flag read by the depletion marking. These are
		// recomputed on every computeRates call and describe the
		// segment that call opens.
		std::vector<double> outflowDemand(stores.size(), 0.0);
		std::vector<double> blockedRate(stores.size(), 0.0);
		std::vector<char> coveredFlag(stores.size(), 0);
		auto const computeRates = [&](double at)
		{
			std::fill(net.begin(), net.end(), 0.0);
			std::fill(maxOutflow.begin(), maxOutflow.end(), 0.0);
			std::fill(limiterId.begin(), limiterId.end(), std::string());
			std::fill(limiterKind.begin(), limiterKind.end(), std::string());
			std::fill(outflowDemand.begin(), outflowDemand.end(), 0.0);
			blocked.clear();
			auto const addOutflow = [&](size_t storeIndex, double rate,
				std::string const& id, char const* kind)
			{
				net[storeIndex] -= rate;
				if (rate > maxOutflow[storeIndex])
				{
					maxOutflow[storeIndex] = rate;
					limiterId[storeIndex] = id;
					limiterKind[storeIndex] = kind;
				}
			};
			// #605: resolve the rates of every projected material
			// pathway - transfers, equipment material exchanges, crew
			// metabolism and manually commanded generator draw - for
			// simultaneous through-flow at dry and full boundary
			// stores before folding them into the net trajectory.
			// Gating each pathway on the inventory at the segment
			// start alone makes any cross-pathway chain oscillate: a
			// dry store fed by equipment stops its outgoing transfer
			// while the equipment fills it, a full store drained by
			// equipment stops the transfer filling it while the
			// equipment drains it, and the solve spends the horizon
			// in alternating half-duty phases that invent shortages
			// and burn the segment budget while the live station
			// flows straight through. #593 settled this for transfer
			// edges only; the same rule now covers every pathway.
			//
			// Every consuming pathway starts at its full authored,
			// profile-scaled rate and the rates settle: a store
			// projected dry may pass out only what arrives
			// concurrently from any pathway, and a store projected
			// full may take in only what leaves concurrently to any
			// pathway. Transfers are capped in the live
			// transfer-priority order; equipment loads and crew
			// metabolism keep their whole-unit semantics - a load
			// whose feed or destination cannot be fully covered
			// stops whole, exactly as the forecast's conservative
			// #554 starvation rule already does, and the claim
			// order mirrors the live tick where a granted load
			// reserves its material ahead of the transfer pass and
			// crew metabolism runs ahead of it too. The settle only
			// ever lowers rates or stops whole units, so it
			// terminates. What a dry store cannot pass out or pay
			// for stays uncovered demand at that store; what a full
			// store cannot take in is back-pressure that leaves the
			// material upstream, not a shortage.
			//
			// Support is recomputed inside the settle over the whole
			// graph, not just transfer edges: a store is supported
			// when it holds drawable material, or when a running
			// pathway could carry material into it from a supported
			// source - a transfer out of a supported store, a load
			// whose own feeds are all supported delivering into it,
			// or a crew with supported food and water returning
			// waste into it. A dry store behind no real supply
			// stays unsupported: its transfers are zeroed and its
			// consuming loads, crews and commanded generators stop
			// whole, so a closed cycle of dry stores and starved
			// machines can never bootstrap itself into phantom
			// material however its rates balance.
			std::vector<double> inflowRate(stores.size(), 0.0);
			std::vector<double> outflowRate(stores.size(), 0.0);
			std::vector<double> supplyCapped(stores.size(), 0.0);
			std::vector<double> resolvedRate(transfers.size(), 0.0);
			for (size_t index = 0; index < transfers.size(); ++index)
				resolvedRate[index] = transfers[index].authored.mode == ResourceTransferMode::Continuous
					? transfers[index].authored.ratePerSecond : 0.0;
			// Candidate rates for the non-transfer pathways at this
			// instant's profile level. A stopped load or crew stops
			// whole and its consuming demand is recorded uncovered
			// at the stores it would have drawn from.
			struct CrewRates
			{
				double food{0.0};
				double water{0.0};
				double urine{0.0};
				double organicWaste{0.0};
			};
			std::vector<CrewRates> crewRates(crews.size());
			std::vector<char> crewRunning(crews.size(), 0);
			for (size_t index = 0; index < crews.size(); ++index)
			{
				auto const& crew = crews[index];
				if (crew.authored.crewCount <= 0) continue;
				auto const level = evaluateStationProfile(
					crew.authored.activityProfile, at);
				if (level <= 0.0) continue;
				auto const scale = static_cast<double>(crew.authored.crewCount)
					* level;
				crewRates[index].food = crew.authored.foodPerCrewPerS * scale;
				crewRates[index].water = crew.authored.waterPerCrewPerS * scale;
				crewRates[index].urine = crew.authored.urinePerCrewPerS * scale;
				crewRates[index].organicWaste
					= crew.authored.organicWastePerCrewPerS * scale;
				crewRunning[index] = 1;
				outflowDemand[crew.foodStoreIndex] += crewRates[index].food;
				outflowDemand[crew.waterStoreIndex] += crewRates[index].water;
			}
			std::vector<double> loadLevel(loads.size(), 0.0);
			std::vector<char> loadRunning(loads.size(), 0);
			for (size_t index = 0; index < loads.size(); ++index)
			{
				auto const& load = loads[index];
				if (load.exchanges.empty()) continue;
				auto const level = evaluateStationProfile(
					load.authored.operatingProfile, at);
				if (level <= 0.0) continue;
				loadLevel[index] = level;
				loadRunning[index] = 1;
				for (auto const& exchange : load.exchanges)
					if (exchange.consumes && exchange.ratePerSecond > 0.0)
						outflowDemand[exchange.storeIndex]
							+= exchange.ratePerSecond * level;
			}
			std::vector<double> generatorDraw(generators.size(), 0.0);
			std::vector<char> generatorRunning(generators.size(), 0);
			// #594: a manually commanded generator is already burning
			// authored fuel: its draw is deterministic demand and is
			// projected like any other pathway - an upper bound on the
			// live burn, which ramp, cooling trips and fuel throttling
			// can only lower - so the projection stays on the safe
			// side of the live outcome. Under Automatic dispatch the
			// draw follows the unmodelled net load, so nothing is
			// projected here and the fuel store is flagged with the
			// other unprojected consuming pathways.
			if (!forecastDispatchAutomatic)
			{
				for (size_t index = 0; index < generators.size(); ++index)
				{
					auto const& generator = generators[index];
					auto const boundKW = std::min(generator.authored.ratedOutputKW,
						generator.authored.commandedOutputKW);
					if (boundKW <= 0.0) continue;
					auto const kwhPerUnit = stationEnergyUnitsToKWh(
						stores[generator.fuelStoreIndex].authored.unit);
					auto const drawUnitsPerS = boundKW
						/ generator.effectiveWearCapability() / 3600.0 / kwhPerUnit;
					if (drawUnitsPerS <= 0.0) continue;
					generatorDraw[index] = drawUnitsPerS;
					generatorRunning[index] = 1;
					outflowDemand[generator.fuelStoreIndex] += drawUnitsPerS;
				}
			}
			auto const stopLoad = [&](size_t index)
			{
				if (!loadRunning[index]) return;
				loadRunning[index] = 0;
				// #554 whole-load starvation: the load's material
				// exchanges are one operating dependency; when the
				// load stops, every consuming exchange's demand stays
				// uncovered at its store.
				for (auto const& exchange : loads[index].exchanges)
					if (exchange.consumes && exchange.ratePerSecond > 0.0)
						blocked.push_back({exchange.storeIndex,
							exchange.ratePerSecond * loadLevel[index]});
			};
			auto const stopCrew = [&](size_t index)
			{
				if (!crewRunning[index]) return;
				crewRunning[index] = 0;
				// Crew metabolism runs whole or not at all: the
				// held-back food and water demand stays uncovered,
				// matching the live uniform-scale rule (#401).
				if (crewRates[index].food > 0.0)
					blocked.push_back({crews[index].foodStoreIndex,
						crewRates[index].food});
				if (crewRates[index].water > 0.0)
					blocked.push_back({crews[index].waterStoreIndex,
						crewRates[index].water});
			};
			auto const stopGenerator = [&](size_t index)
			{
				if (!generatorRunning[index]) return;
				generatorRunning[index] = 0;
				blocked.push_back({generators[index].fuelStoreIndex,
					generatorDraw[index]});
			};
			auto const recomputeFlows = [&]
			{
				std::fill(inflowRate.begin(), inflowRate.end(), 0.0);
				std::fill(outflowRate.begin(), outflowRate.end(), 0.0);
				for (size_t index = 0; index < transfers.size(); ++index)
				{
					inflowRate[transfers[index].destinationIndex]
						+= resolvedRate[index];
					outflowRate[transfers[index].sourceIndex]
						+= resolvedRate[index];
				}
				for (size_t index = 0; index < crews.size(); ++index)
				{
					if (!crewRunning[index]) continue;
					outflowRate[crews[index].foodStoreIndex]
						+= crewRates[index].food;
					outflowRate[crews[index].waterStoreIndex]
						+= crewRates[index].water;
					inflowRate[crews[index].urineStoreIndex]
						+= crewRates[index].urine;
					inflowRate[crews[index].organicWasteStoreIndex]
						+= crewRates[index].organicWaste;
				}
				for (size_t index = 0; index < loads.size(); ++index)
				{
					if (!loadRunning[index]) continue;
					for (auto const& exchange : loads[index].exchanges)
					{
						if (exchange.ratePerSecond <= 0.0) continue;
						auto const rate = exchange.ratePerSecond
							* loadLevel[index];
						if (exchange.consumes)
							outflowRate[exchange.storeIndex] += rate;
						else
							inflowRate[exchange.storeIndex] += rate;
					}
				}
				for (size_t index = 0; index < generators.size(); ++index)
				{
					if (!generatorRunning[index]) continue;
					outflowRate[generators[index].fuelStoreIndex]
						+= generatorDraw[index];
				}
			};
			// Every pass either settles or strictly lowers some rate
			// or stops some whole unit; nothing ever rises, so the
			// solve terminates. Each stop or cap propagates at most
			// one dependency hop per pass, and the whole run holds
			// at most one stop per actor and one cap wave per hop,
			// so the product below bounds the passes with room to
			// spare.
			for (size_t pass = 0;
				pass < (loads.size() + crews.size() + generators.size() + 2)
					* (transfers.size() + stores.size() + 2); ++pass)
			{
				bool settled = true;
				// Support fixed point over every pathway.
				std::vector<char> supported(stores.size(), 0);
				for (size_t store = 0; store < stores.size(); ++store)
					if (!isDry(store)) supported[store] = 1;
				for (bool grew = true; grew; )
				{
					grew = false;
					for (auto const index : transferOrder)
					{
						if (resolvedRate[index] <= 0.0) continue;
						if (!supported[transfers[index].sourceIndex]) continue;
						if (supported[transfers[index].destinationIndex]) continue;
						supported[transfers[index].destinationIndex] = 1;
						grew = true;
					}
					for (size_t index = 0; index < loads.size(); ++index)
					{
						if (!loadRunning[index]) continue;
						bool feedsSupported = true;
						for (auto const& exchange : loads[index].exchanges)
							if (exchange.consumes
								&& exchange.ratePerSecond > 0.0
								&& !supported[exchange.storeIndex])
								feedsSupported = false;
						if (!feedsSupported) continue;
						for (auto const& exchange : loads[index].exchanges)
						{
							if (exchange.consumes
								|| exchange.ratePerSecond <= 0.0) continue;
							if (supported[exchange.storeIndex]) continue;
							supported[exchange.storeIndex] = 1;
							grew = true;
						}
					}
					for (size_t index = 0; index < crews.size(); ++index)
					{
						if (!crewRunning[index]) continue;
						if (!supported[crews[index].foodStoreIndex]
							|| !supported[crews[index].waterStoreIndex])
							continue;
						if (!supported[crews[index].urineStoreIndex])
						{
							supported[crews[index].urineStoreIndex] = 1;
							grew = true;
						}
						if (!supported[crews[index].organicWasteStoreIndex])
						{
							supported[crews[index].organicWasteStoreIndex] = 1;
							grew = true;
						}
					}
				}
				// Nothing real can flow out of an unsupported store.
				for (size_t index = 0; index < transfers.size(); ++index)
				{
					if (resolvedRate[index] <= 0.0) continue;
					if (supported[transfers[index].sourceIndex]) continue;
					supplyCapped[transfers[index].sourceIndex]
						+= resolvedRate[index];
					resolvedRate[index] = 0.0;
					settled = false;
				}
				for (size_t index = 0; index < loads.size(); ++index)
				{
					if (!loadRunning[index]) continue;
					for (auto const& exchange : loads[index].exchanges)
						if (exchange.consumes && exchange.ratePerSecond > 0.0
							&& !supported[exchange.storeIndex])
						{
							stopLoad(index);
							settled = false;
							break;
						}
				}
				for (size_t index = 0; index < crews.size(); ++index)
				{
					if (!crewRunning[index]) continue;
					if (!supported[crews[index].foodStoreIndex]
						|| !supported[crews[index].waterStoreIndex])
					{
						stopCrew(index);
						settled = false;
					}
				}
				for (size_t index = 0; index < generators.size(); ++index)
				{
					if (!generatorRunning[index]) continue;
					if (!supported[generators[index].fuelStoreIndex])
					{
						stopGenerator(index);
						settled = false;
					}
				}
				recomputeFlows();
				// Boundary budgets: a dry store passes out only what
				// arrives concurrently, a full store takes in only
				// what leaves concurrently. Equipment loads claim
				// first, then crew, then commanded generators, then
				// transfers in the authored priority order,
				// mirroring the live tick where a granted load
				// reserves its feed and destination and the crew
				// draws ahead of the transfer pass.
				for (size_t store = 0; store < stores.size(); ++store)
				{
					if (isDry(store))
					{
						double budget = inflowRate[store];
						for (size_t index = 0; index < loads.size(); ++index)
						{
							if (!loadRunning[index]) continue;
							double draw = 0.0;
							for (auto const& exchange : loads[index].exchanges)
								if (exchange.consumes
									&& exchange.storeIndex == store)
									draw += exchange.ratePerSecond
										* loadLevel[index];
							if (draw <= 0.0) continue;
							auto const tol = StationBalanceTolerance
								* std::max(1.0, draw);
							if (draw > budget + tol)
							{
								stopLoad(index);
								settled = false;
								continue;
							}
							// A claim admitted within the balance
							// tolerance can leave the budget a hair
							// negative; clamp so a later min() can
							// never resolve a negative rate.
							budget = std::max(0.0, budget - draw);
						}
						for (size_t index = 0; index < crews.size(); ++index)
						{
							if (!crewRunning[index]) continue;
							auto const draw = crews[index].foodStoreIndex
								== store ? crewRates[index].food
								: (crews[index].waterStoreIndex == store
									? crewRates[index].water : 0.0);
							if (draw <= 0.0) continue;
							auto const tol = StationBalanceTolerance
								* std::max(1.0, draw);
							if (draw > budget + tol)
							{
								stopCrew(index);
								settled = false;
								continue;
							}
							// A claim admitted within the balance
							// tolerance can leave the budget a hair
							// negative; clamp so a later min() can
							// never resolve a negative rate.
							budget = std::max(0.0, budget - draw);
						}
						for (size_t index = 0; index < generators.size();
							++index)
						{
							if (!generatorRunning[index]) continue;
							if (generators[index].fuelStoreIndex != store)
								continue;
							auto const draw = generatorDraw[index];
							auto const tol = StationBalanceTolerance
								* std::max(1.0, draw);
							if (draw > budget + tol)
							{
								stopGenerator(index);
								settled = false;
								continue;
							}
							// A claim admitted within the balance
							// tolerance can leave the budget a hair
							// negative; clamp so a later min() can
							// never resolve a negative rate.
							budget = std::max(0.0, budget - draw);
						}
						for (auto const index : transferOrder)
						{
							if (transfers[index].sourceIndex != store) continue;
							auto const allowed = std::min(resolvedRate[index],
								budget);
							budget -= allowed;
							if (allowed < resolvedRate[index])
							{
								supplyCapped[store]
									+= resolvedRate[index] - allowed;
								resolvedRate[index] = allowed;
								settled = false;
							}
						}
					}
					if (isFull(store))
					{
						double budget = outflowRate[store];
						for (size_t index = 0; index < loads.size(); ++index)
						{
							if (!loadRunning[index]) continue;
							double deliver = 0.0;
							for (auto const& exchange : loads[index].exchanges)
								if (!exchange.consumes
									&& exchange.storeIndex == store)
									deliver += exchange.ratePerSecond
										* loadLevel[index];
							if (deliver <= 0.0) continue;
							auto const tol = StationBalanceTolerance
								* std::max(1.0, deliver);
							if (deliver > budget + tol)
							{
								stopLoad(index);
								settled = false;
								continue;
							}
							budget = std::max(0.0, budget - deliver);
						}
						for (size_t index = 0; index < crews.size(); ++index)
						{
							if (!crewRunning[index]) continue;
							auto const deliver = crews[index].urineStoreIndex
								== store ? crewRates[index].urine
								: (crews[index].organicWasteStoreIndex
									== store ? crewRates[index].organicWaste
										: 0.0);
							if (deliver <= 0.0) continue;
							auto const tol = StationBalanceTolerance
								* std::max(1.0, deliver);
							if (deliver > budget + tol)
							{
								stopCrew(index);
								settled = false;
								continue;
							}
							budget = std::max(0.0, budget - deliver);
						}
						for (auto const index : transferOrder)
						{
							if (transfers[index].destinationIndex != store)
								continue;
							auto const allowed = std::min(resolvedRate[index],
								budget);
							budget -= allowed;
							if (allowed < resolvedRate[index])
							{
								resolvedRate[index] = allowed;
								settled = false;
							}
						}
					}
				}
				if (settled) break;
			}
			// Re-derive the concurrent in/out totals from the settled
			// rates so the coverage test below reads the final rates
			// even if the pass bound was reached without settling.
			recomputeFlows();
			// Fold the settled rates into the net trajectory.
			for (size_t index = 0; index < transfers.size(); ++index)
			{
				auto const& transfer = transfers[index];
				if (transfer.authored.mode == ResourceTransferMode::Scheduled || transfer.authored.ratePerSecond <= 0.0) continue;
				outflowDemand[transfer.sourceIndex]
					+= transfer.authored.ratePerSecond;
				auto const rate = resolvedRate[index];
				if (rate <= 0.0) continue;
				net[transfer.sourceIndex] -= rate;
				net[transfer.destinationIndex] += rate;
				if (rate > maxOutflow[transfer.sourceIndex])
				{
					maxOutflow[transfer.sourceIndex] = rate;
					limiterId[transfer.sourceIndex] = transfer.authored.id;
					limiterKind[transfer.sourceIndex] = "transfer";
				}
			}
			for (size_t index = 0; index < crews.size(); ++index)
			{
				if (!crewRunning[index]) continue;
				auto const& crew = crews[index];
				addOutflow(crew.foodStoreIndex, crewRates[index].food,
					crew.authored.id, "crew");
				addOutflow(crew.waterStoreIndex, crewRates[index].water,
					crew.authored.id, "crew");
				net[crew.urineStoreIndex] += crewRates[index].urine;
				net[crew.organicWasteStoreIndex]
					+= crewRates[index].organicWaste;
			}
			for (size_t index = 0; index < loads.size(); ++index)
			{
				if (!loadRunning[index]) continue;
				for (auto const& exchange : loads[index].exchanges)
				{
					if (exchange.ratePerSecond <= 0.0) continue;
					auto const rate = exchange.ratePerSecond * loadLevel[index];
					if (exchange.consumes)
						addOutflow(exchange.storeIndex, rate,
							loads[index].authored.id, "equipment");
					else
						net[exchange.storeIndex] += rate;
				}
			}
			for (size_t index = 0; index < generators.size(); ++index)
			{
				if (!generatorRunning[index]) continue;
				addOutflow(generators[index].fuelStoreIndex,
					generatorDraw[index], generators[index].authored.id,
					"generator");
			}
			// A dry source that could not pass its full demand leaves
			// the shortfall as uncovered demand at that store.
			for (size_t store = 0; store < stores.size(); ++store)
				if (supplyCapped[store] > 0.0)
					blocked.push_back({store, supplyCapped[store]});
			// #593/#605: through-flow coverage. A store whose outflow
			// demand - transfer, equipment, crew or commanded
			// generator draw - is nonzero, fully running (nothing of
			// it blocked) and fed by at least as much concurrent
			// inflow from any pathway is a pass-through, not a
			// shortage: it can sit dry at the boundary for the whole
			// horizon while continuously satisfying its outflow,
			// exactly as the live station does.
			std::fill(blockedRate.begin(), blockedRate.end(), 0.0);
			for (auto const& flow : blocked)
				blockedRate[flow.storeIndex] += flow.rate;
			std::fill(coveredFlag.begin(), coveredFlag.end(), 0);
			for (size_t store = 0; store < stores.size(); ++store)
			{
				auto const demand = outflowDemand[store];
				if (demand <= 0.0) continue;
				auto const tol = StationBalanceTolerance
					* std::max(1.0, demand);
				if (blockedRate[store] > tol) continue;
				if (inflowRate[store] + tol < demand) continue;
				coveredFlag[store] = 1;
			}
		};
		// A store that starts dry is short unless its outflow demand
		// is fully covered by concurrent through-flow from any
		// projected pathway (#593, extended to every pathway by
		// #605): a pass-through intermediate that never holds
		// material but always passes what arrives is not a
		// shortage, so it must not read as a shortfall that starts
		// with the forecast.
		computeRates(asOfSeconds);
		for (size_t index = 0; index < stores.size(); ++index)
			if (isDry(index) && !coveredFlag[index])
			{
				projected[index].depleted = true;
				projected[index].depletionOnset = asOfSeconds;
			}
		std::vector<std::vector<StationForecastArrival>> arrivalsByStore(
			stores.size());
		double time = asOfSeconds;
		size_t eventIndex = 0;
		size_t breakpointIndex = 0;
		std::uint32_t segments = 0;
		std::vector<double> netAtStart(stores.size(), 0.0);
		bool firstSegment = true;
		// #604: the projection may not run past the earliest event the
		// budget could not include. Everything strictly before this
		// bound is modelled completely; everything beyond it is
		// explicitly unavailable, never silently projected over.
		double const completeLimit = truncated
			? std::min(horizonEnd, firstOmittedSeconds)
			: horizonEnd;
		while (time < completeLimit)
		{
			if (segments >= StationForecastMaxSegments)
			{
				truncated = true;
				break;
			}
			double nextTime = completeLimit;
			if (breakpointIndex < breakpoints.size()
				&& breakpoints[breakpointIndex] < nextTime)
				nextTime = breakpoints[breakpointIndex];
			if (eventIndex < events.size()
				&& events[eventIndex].key.occurrenceSeconds < nextTime)
				nextTime = events[eventIndex].key.occurrenceSeconds;
			computeRates(time);
			if (firstSegment)
			{
				netAtStart = net;
				firstSegment = false;
			}
			// Split the segment at the earliest projected crossing: a
			// store running out or a store filling up.
			double cut = nextTime;
			for (size_t index = 0; index < stores.size(); ++index)
			{
				if (net[index] < 0.0)
				{
					auto const drawable = drawableOf(index);
					if (!isDry(index))
					{
						auto const crossing = time + drawable / -net[index];
						if (crossing < cut) cut = crossing;
					}
				}
				else if (net[index] > 0.0)
				{
					auto const headroom = headroomOf(index);
					if (!isFull(index))
					{
						auto const crossing = time + headroom / net[index];
						if (crossing < cut) cut = crossing;
					}
				}
			}
			if (cut < time) cut = time;
			auto const duration = cut - time;
			if (duration > 0.0)
			{
				++segments;
				for (size_t index = 0; index < stores.size(); ++index)
				{
					auto const before = projected[index].qualified;
					projected[index].qualified += net[index] * duration;
					auto const reserve = stores[index].authored.protectedReserve;
					if (net[index] < 0.0 && projected[index].qualified < reserve)
					{
						// The segment overran the drawable: only what was
						// drawable moved; the remainder is uncovered
						// demand, never negative inventory.
						projected[index].unmetOutflow += reserve
							- projected[index].qualified;
						projected[index].qualified = reserve;
					}
					auto const delta = projected[index].qualified - before;
					if (delta > 0.0) projected[index].inflowMoved += delta;
					else projected[index].outflowMoved += -delta;
				}
				for (auto const& flow : blocked)
					projected[flow.storeIndex].unmetOutflow += flow.rate * duration;
				time = cut;
			}
			else if (nextTime > time) time = nextTime;
			else break;
			// Apply every event that has come due at this instant:
			// arrivals first, then ejections, in the shared scenario
			// ordering (#392/#403).
			while (eventIndex < events.size()
				&& events[eventIndex].key.occurrenceSeconds
					<= time + StationScheduleTolerance)
			{
				auto const& event = events[eventIndex];
				if (event.isDelivery)
				{
					auto const& deliveryState = deliveries[event.configIndex];
					auto const& delivery = deliveryState.authored;
					auto const storeIndex = deliveryState.storeIndex;
					auto& store = projected[storeIndex];
					auto const drawableBefore = drawableOf(storeIndex);
					auto const physicalBefore = store.qualified + store.unqualified;
					auto const accepted = std::min(delivery.quantity,
						headroomOf(storeIndex));
					if (delivery.arrivesQualified)
					{
						store.qualified += accepted;
						store.acceptedQualified += accepted;
					}
					else
					{
						store.unqualified += accepted;
						store.acceptedUnqualified += accepted;
					}
					StationForecastArrival arrival;
					arrival.deliveryId = delivery.id;
					arrival.storeId = delivery.storeId;
					arrival.arrivalSeconds = event.key.occurrenceSeconds;
					arrival.scheduledQuantity = delivery.quantity;
					arrival.acceptedQuantity = accepted;
					arrival.rejectedQuantity = delivery.quantity - accepted;
					arrival.projectedDrawableBefore = drawableBefore;
					arrival.projectedPhysicalBefore = physicalBefore;
					arrival.arrivesQualified = delivery.arrivesQualified;
					// #595: a rescue is judged against the projected
					// trajectory, not the drawable inventory at the
					// arrival instant. The depletion marking runs after
					// the instant's arrivals (arrivals precede the tick's
					// drains), so a flag that is already set here means a
					// shortfall was projected strictly before this
					// arrival; no flag means the arrival is at or ahead
					// of the first projected shortfall - including the
					// exactly-timely delivery that lands drawable-zero
					// and prevents the shortage it arrives into. Once a
					// shortfall has occurred the store stays "late" for
					// every later arrival even if an earlier delivery
					// restored a positive drawable; the immediately
					// preceding inventory alone cannot tell whether the
					// shortage already happened.
					arrival.rescuesStore = !projected[storeIndex].depleted;
					arrivalsByStore[storeIndex].push_back(std::move(arrival));
				}
				else
				{
					auto const& ejectionState = wasteEjections[event.configIndex];
					auto const& ejection = ejectionState.authored;
					auto const storeIndex = ejectionState.storeIndex;
					auto& store = projected[storeIndex];
					auto const qualifiedEjected = ejection.eligibility == WasteEjectionEligibility::HeldOnly ? 0.0
						: std::min(ejection.quantity, std::max(0.0, store.qualified));
					auto const heldEjected = ejection.eligibility == WasteEjectionEligibility::QualifiedOnly ? 0.0
						: std::min(ejection.quantity - qualifiedEjected, std::max(0.0, store.unqualified));
					auto const ejected = qualifiedEjected + heldEjected;
					store.qualified -= qualifiedEjected;
					store.unqualified -= heldEjected;
					store.outflowMoved += ejected;
					// What the store could not eject is uncovered
					// disposal demand, reported, never dropped.
					store.unmetOutflow += ejection.quantity - ejected;
				}
				++eventIndex;
			}
			while (breakpointIndex < breakpoints.size()
				&& breakpoints[breakpointIndex] <= time + StationScheduleTolerance)
				++breakpointIndex;
			// Mark depletion only after the instant's arrivals landed:
			// a delivery due at the same second the store would run dry
			// rescues it, matching the live tick order where arrivals
			// run ahead of this tick's drains.
			for (size_t index = 0; index < stores.size(); ++index)
				if (!projected[index].depleted && isDry(index)
					&& duration > 0.0 && !coveredFlag[index])
				{
					projected[index].depleted = true;
					projected[index].depletionOnset = time;
					projected[index].limiterId = limiterId[index];
					projected[index].limiterKind = limiterKind[index];
				}
		}
		// Uncertainty flags: recovery the scenario deliberately does
		// not count as guaranteed.
		std::set<std::string> biologyRecoveryStores;
		for (auto const& crop : settings.cropPopulations)
		{
			biologyRecoveryStores.insert(crop.foodStoreId);
			biologyRecoveryStores.insert(crop.wasteStoreId);
		}
		for (auto const& culture : settings.algaeCultures)
		{
			biologyRecoveryStores.insert(culture.foodStoreId);
			biologyRecoveryStores.insert(culture.recoveryStoreId);
			biologyRecoveryStores.insert(culture.wasteStoreId);
		}
		for (auto const& processor : settings.microbialProcessors)
		{
			biologyRecoveryStores.insert(processor.nutrientStoreId);
			biologyRecoveryStores.insert(processor.recoveredWaterStoreId);
			biologyRecoveryStores.insert(processor.residueStoreId);
		}
		std::set<std::string> processProductionStores;
		for (auto const& conversion : settings.conversions)
			for (auto const& output : conversion.outputs)
				processProductionStores.insert(output.storeId);
		for (auto const& collector : settings.waterCollectors)
			processProductionStores.insert(collector.destStoreId);
		for (auto const& treatment : settings.waterTreatments)
			processProductionStores.insert(treatment.treatedStoreId);
		for (auto const& cell : settings.electrolyzers)
		{
			processProductionStores.insert(cell.oxygenStoreId);
			processProductionStores.insert(cell.hydrogenStoreId);
		}
		for (auto const& capture : settings.co2Captures)
			processProductionStores.insert(capture.captureStoreId);
		for (auto const& cell : settings.fuelCells)
			processProductionStores.insert(cell.waterStoreId);
		for (auto const& reactor : settings.sabatierReactors)
		{
			processProductionStores.insert(reactor.methaneStoreId);
			processProductionStores.insert(reactor.waterStoreId);
		}
		for (auto const& processor : settings.methaneProcessors)
		{
			processProductionStores.insert(processor.hydrogenStoreId);
			processProductionStores.insert(processor.carbonStoreId);
		}
		// #594: consuming pathways the projection deliberately does
		// not count. Every store these pathways draw from is flagged so
		// a Sustained basis never silently hides an omitted drain.
		// Manually commanded generator fuel draw is projected above as
		// deterministic demand, so a manual generator's fuel store is
		// not flagged for that draw; under Automatic dispatch the draw
		// follows the unmodelled net load and the fuel store is
		// flagged like the other unprojected consumers.
		std::set<std::string> processConsumptionStores;
		if (forecastDispatchAutomatic)
			for (auto const& generator : settings.generators)
				if (generator.ratedOutputKW > 0.0)
					processConsumptionStores.insert(generator.fuelStoreId);
		for (auto const& cell : settings.fuelCells)
		{
			processConsumptionStores.insert(cell.hydrogenStoreId);
			processConsumptionStores.insert(cell.oxygenStoreId);
		}
		for (auto const& treatment : settings.waterTreatments)
		{
			processConsumptionStores.insert(treatment.sourceStoreId);
			processConsumptionStores.insert(treatment.consumableStoreId);
		}
		for (auto const& cell : settings.electrolyzers)
			processConsumptionStores.insert(cell.waterStoreId);
		for (auto const& capture : settings.co2Captures)
			if (!capture.consumableStoreId.empty())
				processConsumptionStores.insert(capture.consumableStoreId);
		for (auto const& reactor : settings.sabatierReactors)
		{
			processConsumptionStores.insert(reactor.co2StoreId);
			processConsumptionStores.insert(reactor.hydrogenStoreId);
		}
		for (auto const& processor : settings.methaneProcessors)
			processConsumptionStores.insert(processor.methaneStoreId);
		for (auto const& conversion : settings.conversions)
			for (auto const& input : conversion.inputs)
				processConsumptionStores.insert(input.storeId);
		for (auto const& job : settings.maintenanceJobs)
			for (auto const& requirement : job.requirements)
				if (requirement.consumed)
					processConsumptionStores.insert(requirement.storeId);
		// Biology uptake is excluded from the projection for the same
		// provisional-biology reason as biology recovery: the stores
		// crops, algae and microbial digesters draw on are flagged so
		// their silent drain cannot read as sustained supply.
		std::set<std::string> biologyConsumptionStores;
		for (auto const& crop : settings.cropPopulations)
		{
			biologyConsumptionStores.insert(crop.waterStoreId);
			biologyConsumptionStores.insert(crop.nitrogenStoreId);
			biologyConsumptionStores.insert(crop.phosphorusStoreId);
			biologyConsumptionStores.insert(crop.potassiumStoreId);
			biologyConsumptionStores.insert(crop.mineralsStoreId);
		}
		for (auto const& culture : settings.algaeCultures)
		{
			biologyConsumptionStores.insert(culture.waterStoreId);
			biologyConsumptionStores.insert(culture.nitrogenStoreId);
			biologyConsumptionStores.insert(culture.phosphorusStoreId);
			biologyConsumptionStores.insert(culture.potassiumStoreId);
			biologyConsumptionStores.insert(culture.mineralsStoreId);
		}
		for (auto const& processor : settings.microbialProcessors)
		{
			biologyConsumptionStores.insert(processor.feedStoreId);
			biologyConsumptionStores.insert(processor.oxygenStoreId);
			biologyConsumptionStores.insert(processor.waterStoreId);
			biologyConsumptionStores.insert(processor.consumableStoreId);
			if (!processor.stockStoreId.empty())
				biologyConsumptionStores.insert(processor.stockStoreId);
		}
		for (size_t index = 0; index < stores.size(); ++index)
		{
			auto const& store = stores[index];
			StationForecastEntry entry;
			entry.storeId = store.authored.id;
			entry.unit = store.authored.unit;
			entry.asOfSeconds = asOfSeconds;
			entry.drawableAtStart = std::max(0.0,
				store.currentQualified - store.authored.protectedReserve);
			entry.physicalAtStart = store.currentQualified + store.currentUnqualified;
			entry.netRateAtStart = netAtStart[index];
			// #604: a shortfall found inside the complete-through range
			// is real - every event before it was modelled - and is
			// reported whatever the truncation. "Sustained" is claimed
			// only for a projection that actually reached the horizon;
			// a truncated projection with no shortfall inside its
			// complete range reports Incomplete, never a promise the
			// unmodelled range cannot break.
			entry.basis = projected[index].depleted
				? StationForecastBasis::Shortfall
				: (truncated ? StationForecastBasis::Incomplete
					: StationForecastBasis::Sustained);
			entry.hasShortfall = projected[index].depleted;
			entry.shortfallOnsetSeconds = projected[index].depleted
				? projected[index].depletionOnset : 0.0;
			entry.limitingServiceId = projected[index].limiterId;
			entry.limitingServiceKind = projected[index].limiterKind;
			entry.projectedDrawableAtHorizon = drawableOf(index);
			entry.unmetOutflowTotal = projected[index].unmetOutflow;
			entry.arrivals = std::move(arrivalsByStore[index]);
			entry.biologyRecoveryPossible
				= biologyRecoveryStores.count(store.authored.id) > 0;
			entry.processProductionPossible
				= processProductionStores.count(store.authored.id) > 0;
			entry.processConsumptionPossible
				= processConsumptionStores.count(store.authored.id) > 0;
			entry.biologyConsumptionPossible
				= biologyConsumptionStores.count(store.authored.id) > 0;
			// Conservation of the projected exchanges: the projected
			// physical total must equal the start plus accepted
			// arrivals plus inflow moved minus outflow moved, within
			// the declared balance tolerance scaled by the store size.
			auto const projectedPhysical
				= projected[index].qualified + projected[index].unqualified;
			entry.conservationResidual = (projectedPhysical - entry.physicalAtStart)
				- (projected[index].inflowMoved + projected[index].acceptedQualified
					+ projected[index].acceptedUnqualified)
				+ projected[index].outflowMoved;
			if (scheduledStores.contains(entry.storeId))
			{
				StationForecastEntry unavailable;
				unavailable.storeId = entry.storeId; unavailable.unit = entry.unit;
				unavailable.asOfSeconds = asOfSeconds;
				unavailable.drawableAtStart = entry.drawableAtStart; unavailable.physicalAtStart = entry.physicalAtStart;
				unavailable.unavailableReason = "Scheduled transfers are not modelled by the declared-profile forecast.";
				entry = std::move(unavailable);
			}
			result.stores.push_back(std::move(entry));
		}
		result.segmentsComputed = segments;
		result.truncated = truncated;
		// #604: the range [asOfSeconds, completeThroughSeconds) is
		// modelled with every authored event and profile breakpoint
		// inside it; whenever this falls short of the horizon the range
		// beyond is unavailable. The loop never advances time past
		// completeLimit, so on a full-horizon run time lands exactly
		// on the horizon end.
		result.completeThroughSeconds = std::min(time, completeLimit);
		return result;
	}

}
