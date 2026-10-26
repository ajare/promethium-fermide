#include "StationSystemsImpl.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace core
{
	namespace
	{
		// #882: how long a transfer Alert stays on the ledger before it clears
		// itself. A transfer is informational - the switch has already moved,
		// and nothing waits on the Alert - so it exists to be read and then to
		// retire on a fixed short window rather than to be acknowledged or to
		// latch beside a fault.
		constexpr double kSupplyTransferAlertSeconds{60.0};
		// The alert type spec §5.2/§8 names. Every fire and clear resolves
		// through the callback registry under this key (ADR-0031), so the
		// transfer Alert is announced the same way every other Alert is and
		// needs no authored Monitor.
		constexpr char const* kSupplyTransferAlertType{"supply-transfer"};
		// A shortfall of roundoff size is not a shortfall. The switch only
		// moves on a shortfall a meter could see, so a primary that can carry
		// its load to within audit noise never trips the switch.
		constexpr double kSupplyTransferEpsilonKW{1e-9};

		// The Alert identity for one load's switch. Direction is part of the
		// identity so a failover and the return that follows it are two
		// records on the ledger rather than one record overwriting the other.
		std::string supplyTransferAlertId(bool toBackup, std::string const& type,
			std::string const& id)
		{
			return std::string{kSupplyTransferAlertType}
				+ (toBackup ? ":to-backup:" : ":to-primary:") + type + ':' + id;
		}
	}

	double StationSystemsRuntime::Impl::Tick::busAvailableFor(std::string const& type,
		std::string const& id, std::string const& busId)
	{
		// What this bus could deliver to this one consumer right now: the
		// generation still unclaimed on the bus plus what the bus's batteries
		// can discharge, all seen through the consumer's own supply
		// connection, so cable loss and a disconnected supply port count
		// exactly as they will when the load actually draws.
		return connections.available(powerPort(type, id),
			generationAvailable(busId) + batteryDeliverableKW(busId));
	}

	bool StationSystemsRuntime::Impl::Tick::transferSupply()
	{
		// #882: the automatic transfer switch.
		//
		// A load authored in Transfer mode owns a primary and a backup Power
		// Bus and sits on exactly one of them. The position is resolved here,
		// once per tick, after every source has registered this tick's
		// generation and before any load has drawn - so a primary that falls
		// short moves the WHOLE load onto its backup in that same tick, and no
		// draw ever straddles both buses. Every consumer's power path already
		// resolves through consumerBus(), so moving the position here is the
		// only change allocation needs: protected priority, battery dispatch,
		// connection loss and the per-bus audit all follow the load with no
		// knowledge that a switch exists.
		//
		// What the switch reads:
		//   * demand - the load's own demand on the pool as it presented it in
		//     the last committed tick, the same supply-independent signal
		//     dispatch runs on (#415). A load that cannot run on its feed
		//     never drives the switch either. The switch needs one tick to see
		//     its load, so a Transfer load whose primary is already dead at
		//     t=0 reads as unmet for that first tick before it moves.
		//   * primary availability - THIS tick's, generation and battery
		//     deliverable included, through the load's own supply connection.
		//     A source that dies at t moves the load at t.
		//
		// Leaving is immediate; coming back is not. The primary must hold the
		// load whole and continuously for the load's declared return dwell
		// before the load returns, and one short tick restarts the clock,
		// because a marginal primary would otherwise flap the switch every
		// tick.
		//
		// There is no third path. A Transfer load resolves only to its declared
		// primary or its declared backup, so a transfer onto a bus that cannot
		// serve leaves the load explicitly unmet on that bus rather than
		// quietly borrowing capacity from somewhere it was never wired to.
		auto visit = [&](char const* type, auto const& entries) {
			for (auto const& entry : entries)
			{
				if (entry.authored.supplyMode != StationSupplyMode::Transfer) continue;
				// Validation refuses a Transfer load with anything but two
				// distinct buses. Anything else has nothing to switch to, so
				// it stays on its declared bus rather than inventing one.
				if (entry.authored.supplyBusIds.size() != 2) continue;
				auto const& primary = entry.authored.supplyBusIds[0];
				auto const& backup = entry.authored.supplyBusIds[1];
				auto const key = std::pair<std::string, std::string>{type, entry.authored.id};
				auto& state = runtime.supplyTransfers[key];
				auto const seen = runtime.lastConsumerDispatchRequest.find(key);
				auto const demand = seen == runtime.lastConsumerDispatchRequest.end()
					? 0.0 : seen->second;
				auto const primaryAvailable = busAvailableFor(type, entry.authored.id, primary);
				auto const canServe = primaryAvailable + kSupplyTransferEpsilonKW >= demand;
				// The instant this tick begins serving on whatever bus it ends
				// up on. Both the dwell clock and the event stamp are held to
				// it, so "30 s on a capable primary" means thirty seconds of
				// the simulation actually ran that way.
				auto const switchInstant = elapsed - seconds;
				if (!state.onBackup)
				{
					// Sitting on the primary. A shortfall of any size moves the
					// whole load now: the switch holds no partial position to
					// fall back on.
					if (canServe) continue;
					state.onBackup = true;
					state.primaryCapableSince.reset();
					state.eventFromBusId = primary;
					state.eventToBusId = backup;
					state.eventAtSimSeconds = switchInstant;
					state.eventDemandKW = demand;
					state.eventPrimaryAvailableKW = primaryAvailable;
					consumerBuses[key] = backup;
					continue;
				}
				// Sitting on the backup. The primary has to prove itself for
				// the whole dwell before the load comes home.
				if (!canServe)
				{
					state.primaryCapableSince.reset();
					continue;
				}
				if (!state.primaryCapableSince.has_value())
					state.primaryCapableSince = switchInstant;
				auto const dwell = std::max(0.0, entry.authored.transferReturnDwellSeconds);
				if (*state.primaryCapableSince + dwell > switchInstant + kSupplyTransferEpsilonKW)
					continue;
				state.onBackup = false;
				state.primaryCapableSince.reset();
				state.eventFromBusId = backup;
				state.eventToBusId = primary;
				state.eventAtSimSeconds = switchInstant;
				state.eventDemandKW = demand;
				state.eventPrimaryAvailableKW = primaryAvailable;
				consumerBuses[key] = primary;
			}
		};
		visit("Equipment", runtime.loads);
		visit("Generator", runtime.generators);
		visit("Coolant loop", runtime.coolantLoops);
		visit("Chiller", runtime.chillers);
		visit("Water treatment", runtime.waterTreatments);
		visit("Electrolyzer", runtime.electrolyzers);
		visit("CO2 capture", runtime.co2Captures);
		visit("Sabatier", runtime.sabatierReactors);
		visit("Methane processor", runtime.methaneProcessors);
		visit("Microbial processor", runtime.microbialProcessors);
		visit("Crops", runtime.cropPopulations);
		visit("Algae", runtime.algaeCultures);
		return true;
	}

	void StationSystemsRuntime::Impl::reconcileTransferAlerts()
	{
		// #882: the Advisory announcement for every transfer the committed
		// tick recorded. This runs outside the ledger beside the Monitor
		// evaluation on purpose. The switch actuated inside the tick; only its
		// announcement waits for the commit. A tick that failed and rolled back
		// took its transfer event with it, so no Alert is ever raised for a
		// move that never happened, and the Alert ledger never needs to be
		// part of the rollback state.
		auto const now = clock.elapsedSeconds();
		AlertSink sink{activeAlerts, closedAlerts, events, clock.steps()};
		for (auto& [key, state] : supplyTransfers)
		{
			if (state.eventAtSimSeconds < 0.0
				|| state.alertRaisedForEventSeconds == state.eventAtSimSeconds)
				continue;
			StationAlertSnapshot alert;
			alert.monitorId = supplyTransferAlertId(state.onBackup, key.first, key.second);
			alert.subjectType = key.first;
			alert.subjectId = key.second;
			alert.metric = kSupplyTransferAlertType;
			// The Alert reads as the measurement that moved the switch: what
			// the load wanted, against what the primary could put on the table.
			alert.currentValue = state.eventDemandKW;
			alert.threshold = state.eventPrimaryAvailableKW;
			alert.severity = AlertSeverity::Advisory;
			alert.provenance = MonitorProvenance::Custom;
			alert.firedAtSeconds = state.eventAtSimSeconds;
			alert.detail = state.eventFromBusId + " -> " + state.eventToBusId;
			state.alertRaisedForEventSeconds = state.eventAtSimSeconds;
			stationAlertCallbacksFor(kSupplyTransferAlertType).onFire(sink, alert);
		}
		// Advisory, not latched: each transfer Alert retires on its own fixed
		// short window whether or not the load has moved again since.
		std::vector<std::string> expired;
		for (auto const& alert : activeAlerts)
			if (alert.metric == kSupplyTransferAlertType
				&& now >= alert.firedAtSeconds + kSupplyTransferAlertSeconds)
				expired.push_back(alert.monitorId);
		for (auto const& monitorId : expired) clearAlert(monitorId, AlertClearReason::AutoClear);
	}
}
