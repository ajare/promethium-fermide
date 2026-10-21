#include "StationBatteryChecks.h"
#include "StationRig.h"

namespace
{
	using namespace smoke::station;
	// Invalid battery configuration is refused before anything runs.
	void batteriesConfig(smoke::Context const&)
	{
		core::World refused("Refused", 2, 2);
		refused.pauseSimulation(); refused.finishBuild(); refused.markSaved();
		for (auto const& bad : {
			battery("", 10, 8, 8, 2, 4),
			battery(std::string(65, 'x'), 10, 8, 8, 2, 4),
			battery("a", -1, 8, 8, 2, 4),
			battery("a", std::numeric_limits<double>::quiet_NaN(), 8, 8, 2, 4),
			battery("a", 10, 11, 8, 2, 4),
			battery("a", 10, -1, 8, 2, 4),
			battery("a", 10, 8, 11, 2, 4),
			battery("a", 10, 8, -1, 2, 4),
			battery("a", 10, 8, 8, -1, 4),
			battery("a", 10, 8, 8, 2, -1),
			battery("a", 10, 8, 8, 2, 4, 0.0, 1.0),
			battery("a", 10, 8, 8, 2, 4, 1.5, 1.0),
			battery("a", 10, 8, 8, 2, 4, 1.0, 0.0),
			battery("a", 10, 8, 8, 2, 4, 1.0, -0.5),
			battery("a", 10, 8, 8, 2, 4, std::numeric_limits<double>::quiet_NaN(), 1.0) })
		{
			auto const [ok, diagnostic] = configure(refused, {}, {}, {bad});
			require(!ok && !diagnostic.empty(), "Invalid battery configuration accepted");
			require(!refused.getStationSystemsSnapshot().enabled && !refused.isModified(),
				"Rejected battery configuration mutated the World");
		}
		{
			std::vector<core::BatterySettings> tooMany;
			for (size_t index = 0; index <= core::MaxStationBatteries; ++index)
				tooMany.push_back(battery("b" + std::to_string(index), 1, 1, 1, 1, 1));
			require(!configure(refused, {}, {}, tooMany).first, "Battery count limit not enforced");
		}
		// #547: analytical kW <-> kWh crossings, computed from first
		// principles rather than from the runtime: 1 kW over one second
		// moves 1/3600 kWh, over one hour exactly 1 kWh. Both directions,
	}

	// Split out of `batteries` so this world runs as its own ctest test rather than in one serial chain.
	void batteriesHourDischarge(smoke::Context const&)
	{
		core::World oneHour("HourDischarge", 2, 2);
		oneHour.pauseSimulation(); oneHour.finishBuild();
		require(configure(oneHour, {},
			{load("lamp", 1.0, 0)},
			{battery("b", 1.0, 1.0, 1.0, 1.0, 1.0)}).first,
			"One-hour discharge scenario refused");
		oneHour.resumeSimulation();
		require(smoke::station::advanceSeconds(oneHour, 3600.01666666667), "One-hour discharge run failed");
		{
			auto const snap = oneHour.getStationSystemsSnapshot();
			auto const& b = snap.power.batteries[0];
			require(hourTight(b.storedKWh, 0.0)
				&& hourTight(b.dischargedKWh, 1.0)
				&& hourTight(b.deliveredKWh, 1.0)
				&& hourTight(b.residualKWh, 0.0)
				&& b.atReserve
				&& approx(snap.power.batteryDischargeKW, 0.0)
				&& approx(snap.power.batteryShortfallKW, 1.0),
				"1 kW over one hour did not move exactly 1 kWh");
		}
	}

	// Split out of `batteries` so this world runs as its own ctest test rather than in one serial chain.
	void batteriesHourCharge(smoke::Context const&)
	{
		core::World oneHourCharge("HourCharge", 2, 2);
		oneHourCharge.pauseSimulation(); oneHourCharge.finishBuild();
		require(configure(oneHourCharge,
			{array("field", 1.0, constantProfile(1.0))},
			{},
			{battery("b", 1.0, 1.0, 0.0, 1.0, 1.0)}).first,
			"One-hour charge scenario refused");
		oneHourCharge.resumeSimulation();
		require(smoke::station::advanceSeconds(oneHourCharge, 3600.01666666667), "One-hour charge run failed");
		{
			auto const snap = oneHourCharge.getStationSystemsSnapshot();
			auto const& b = snap.power.batteries[0];
			require(hourTight(b.storedKWh, 1.0)
				&& hourTight(b.chargedKWh, 1.0)
				&& hourTight(b.chargeInputKWh, 1.0)
				&& hourTight(b.residualKWh, 0.0)
				&& b.saturated
				&& approx(snap.power.batteryChargeKW, 0.0)
				&& approx(snap.power.surplusKW, 1.0),
				"1 kW over one hour did not store exactly 1 kWh or saturation not reached");
		}
	}

	// Split out of `batteries` so this world runs as its own ctest test rather than in one serial chain.
	void batteriesHourChargeLoss(smoke::Context const&)
	{
		core::World hourChargeLoss("HourChargeLoss", 2, 2);
		hourChargeLoss.pauseSimulation(); hourChargeLoss.finishBuild();
		require(configure(hourChargeLoss,
			{array("field", 1.0, constantProfile(1.0))},
			{},
			{battery("b", 2.0, 2.0, 0.0, 1.0, 1.0, 0.5, 1.0)}).first,
			"One-hour charge-loss scenario refused");
		hourChargeLoss.resumeSimulation();
		require(smoke::station::advanceSeconds(hourChargeLoss, 3600.0), "One-hour charge-loss run failed");
		{
			auto const b = hourChargeLoss.getStationSystemsSnapshot().power.batteries[0];
			require(hourTight(b.storedKWh, 0.5)
				&& hourTight(b.chargedKWh, 0.5)
				&& hourTight(b.chargeInputKWh, 1.0)
				&& hourTight(b.lossesKWh, 0.5)
				&& hourTight(b.residualKWh, 0.0),
				"0.5 charge efficiency over one hour booked the wrong amounts");
		}
	}

	// Split out of `batteries` so this world runs as its own ctest test rather than in one serial chain.
	void batteriesHourDischargeLoss(smoke::Context const&)
	{
		core::World hourDischargeLoss("HourDischargeLoss", 2, 2);
		hourDischargeLoss.pauseSimulation(); hourDischargeLoss.finishBuild();
		require(configure(hourDischargeLoss, {},
			{load("lamp", 1.0, 0)},
			{battery("b", 4.0, 4.0, 4.0, 1.0, 1.0, 1.0, 0.5)}).first,
			"One-hour discharge-loss scenario refused");
		hourDischargeLoss.resumeSimulation();
		require(smoke::station::advanceSeconds(hourDischargeLoss, 3600.0), "One-hour discharge-loss run failed");
		{
			auto const b = hourDischargeLoss.getStationSystemsSnapshot().power.batteries[0];
			require(hourTight(b.storedKWh, 2.0)
				&& hourTight(b.dischargedKWh, 2.0)
				&& hourTight(b.deliveredKWh, 1.0)
				&& hourTight(b.lossesKWh, 1.0)
				&& hourTight(b.residualKWh, 0.0),
				"0.5 discharge efficiency over one hour booked the wrong amounts");
		}
	}

	// Split out of `batteries` so this world runs as its own ctest test rather than in one serial chain.
	void batteriesCharging(smoke::Context const&)
	{
		core::World charging("Charging", 2, 2);
		charging.pauseSimulation(); charging.finishBuild();
		require(configure(charging,
			{array("field", 10.0, constantProfile(1.0))},
			{load("shop", 6.0, 0)},
			{battery("bank", 10.0, 8.0, 2.0, 2.0, 4.0)}).first, "Charging scenario refused");
		charging.markSaved();
		charging.resumeSimulation();
		require(smoke::station::advanceSeconds(charging, 3599.98333333), "Charging run failed");
		{
			auto const snap = charging.getStationSystemsSnapshot();
			require(snap.power.batteries.size() == 1
				&& approx(snap.power.batteries[0].storedKWh, 4.0)
				&& approx(snap.power.batteryChargeKW, 2.0)
				&& approx(snap.power.batteryDischargeKW, 0.0)
				&& approx(snap.power.surplusKW, 2.0),
				"Surplus charging did not take exactly the untaken generation");
			require(snap.power.batteries[0].reserveKWh == 2.0
				&& snap.power.batteries[0].usableKWh == 8.0
				&& approx(snap.power.batteries[0].stateOfCharge, 0.4),
				"Reserve policy not visible in the snapshot");
			require(!charging.isModified(), "Battery charging dirtied the authored document");
		}
		require(smoke::station::advanceSeconds(charging, 10800.05), "Charging-to-full run failed");
		{
			auto const snap = charging.getStationSystemsSnapshot();
			require(approx(snap.power.batteries[0].storedKWh, 10.0)
				&& snap.power.batteries[0].saturated
				&& approx(snap.power.batteryChargeKW, 0.0)
				&& approx(snap.power.surplusKW, 4.0),
				"Saturated battery still drew charge or surplus not restored");
			require(approx(snap.power.batteries[0].chargedKWh, 8.0)
				&& approx(snap.power.batteries[0].chargeInputKWh, 8.0)
				&& approx(snap.power.batteries[0].residualKWh, 0.0)
				&& approx(snap.power.batteries[0].lossesKWh, 0.0),
				"Charge ledger did not close");
			require(sawEvent(charging, "Battery 'bank' fully charged: stored energy reached nameplate capacity"),
				"Saturation transition missing from the event log");
		}
	}

	// Split out of `batteries` so this world runs as its own ctest test rather than in one serial chain.
	void batteriesEclipse(smoke::Context const&)
	{
		core::World eclipse("Eclipse", 2, 2);
		eclipse.pauseSimulation(); eclipse.finishBuild();
		{
			core::ProfileSettings illumination;
			illumination.periodSeconds = 10800.0;
			illumination.segments = {{0.0, 1.0}, {5400.0, 0.0}};
			// Nameplate 6, usable 4: the reserve floor is 2 kWh. One hour
			// of sun adds 2 kWh at the 2 kW charge limit; one hour of
			// eclipse drains 4 kWh at the 4 kW discharge limit.
			require(configure(eclipse,
				{array("orbital", 10.0, illumination)},
				{load("station", 6.0, 0)},
				{battery("bank", 6.0, 4.0, 4.0, 2.0, 4.0)}).first, "Eclipse battery scenario refused");
			eclipse.resumeSimulation();
			// Half an hour of sun: the battery has taken exactly 1 kWh.
			require(smoke::station::advanceSeconds(eclipse, 1800.0), "Eclipse sun leg failed");
			{
				auto const snap = eclipse.getStationSystemsSnapshot();
				require(hourTight(snap.power.batteries[0].storedKWh, 5.0)
					&& tight(snap.power.batteryChargeKW, 2.0)
					&& tight(snap.power.surplusKW, 2.0),
					"Battery did not charge 1 kWh in half an hour of sun");
			}
			// A full hour of sun: the battery reaches nameplate exactly.
			require(smoke::station::advanceSeconds(eclipse, 1800.03333333), "Sun top-up leg failed");
			{
				auto const snap = eclipse.getStationSystemsSnapshot();
				require(hourTight(snap.power.batteries[0].storedKWh, 6.0)
					&& snap.power.batteries[0].saturated,
					"Battery did not charge to nameplate in one hour of sun");
			}
			// One settled sun tick with no headroom: nothing is drawn and
			// the whole surplus stays visible.
			require(advanceStationSteps(eclipse, 1), "Saturated sun step failed");
			{
				auto const snap = eclipse.getStationSystemsSnapshot();
				require(approx(snap.power.batteryChargeKW, 0.0)
					&& approx(snap.power.surplusKW, 4.0),
					"Saturated battery still drew charge or surplus not restored");
			}
			// Half an hour into eclipse: the battery carries 4 kW of the
			// 6 kW load through the shared pool and has given exactly 2 kWh.
			require(smoke::station::advanceSeconds(eclipse, 3599.93333333), "Eclipse ride-through leg failed");
			{
				auto const snap = eclipse.getStationSystemsSnapshot();
				require(approx(snap.power.availableGenerationKW, 0.0)
					&& hourTight(snap.power.batteries[0].storedKWh,
						6.0 - 4.0 * (snap.elapsedSeconds - 5400.0) / 3600.0)
					&& approx(snap.power.batteryDischargeKW, 4.0)
					&& approx(snap.power.batteryShortfallKW, 2.0),
					"Battery did not ride the eclipse deficit through the shared pool");
				require(snap.power.loads[0].status == core::StationTransferStatus::Partial
					&& approx(snap.power.loads[0].suppliedKW, 4.0)
					&& approx(snap.power.loads[0].unmetKW, 2.0)
					&& approx(snap.power.totalSuppliedKW, 4.0),
					"Load service during battery ride-through not integrated");
				require(snap.power.totalSuppliedKW
					<= snap.power.availableGenerationKW + snap.power.batteryDischargeKW + 1e-9,
					"Integrated supply exceeded generation plus battery output");
				require(snap.power.batteries[0].status == core::StationTransferStatus::Partial,
					"Power-limited discharge not visible in the battery status");
			}
			// One full hour into eclipse: the usable energy is gone; the
			// reserve floor stops the discharge exactly at 2 kWh.
			require(smoke::station::advanceSeconds(eclipse, 1800.18333333), "Eclipse exhaustion leg failed");
			{
				auto const snap = eclipse.getStationSystemsSnapshot();
				require(hourTight(snap.power.batteries[0].storedKWh, 2.0)
					&& snap.power.batteries[0].atReserve,
					"Battery did not stop exactly at its reserve floor");
				require(sawEvent(eclipse, "Battery 'bank' exhausted: usable energy depleted to the reserve floor"),
					"Exhaustion transition missing from the event log");
			}
			// The rest of the eclipse: the exhausted battery delivers
			// nothing and the whole load stays unmet with the shortfall
			// visible.
			require(advanceToLastStepBefore(eclipse, 10800.0), "Post-exhaustion leg failed");
			{
				auto const snap = eclipse.getStationSystemsSnapshot();
				require(approx(snap.power.batteries[0].storedKWh, 2.0)
					&& approx(snap.power.batteryDischargeKW, 0.0)
					&& approx(snap.power.batteryShortfallKW, 6.0),
					"Exhausted battery still delivered energy or shortfall not visible");
				require(snap.power.loads[0].status == core::StationTransferStatus::Rejected
					&& approx(snap.power.loads[0].suppliedKW, 0.0),
					"Load not fully unmet after battery exhaustion");
			}
			// Sunrise: charging resumes and the battery recovers above the
			// reserve floor.
			require(advanceStationSteps(eclipse, 1), "Sunrise step failed");
			{
				auto const snap = eclipse.getStationSystemsSnapshot();
				require(!snap.power.batteries[0].atReserve
					&& snap.power.batteryChargeKW > 0.0,
					"Battery did not recover and recharge at sunrise");
				require(sawEvent(eclipse, "Battery 'bank' recovered above the reserve floor"),
					"Recovery transition missing from the event log");
			}
			// Half an hour of sun brings it back to 3 kWh, then replay
			// the whole run identically after reset.
			require(advanceSeconds(eclipse, 12600.0 - eclipse.getSimulationTick()
				* static_cast<double>(core::World::getFixedTimestep())), "Cycle continuation failed");
			eclipse.pauseSimulation();
			auto const expected = eclipse.getStationSystemsSnapshot();
			auto const expectedEvents = eclipse.getStationSystemsEvents();
			auto const replayTicks = eclipse.getSimulationTick();
			require(expected.worldTick == (replayTicks + expected.systemsStepWorldTicks - 1)
				/ expected.systemsStepWorldTicks, "Cycle run did not reach the expected Station step");
			require(hourTight(expected.power.batteries[0].storedKWh,
				2.0 + 2.0 * (expected.elapsedSeconds - 10800.0) / 3600.0),
				"Recovery leg did not store 1 kWh");
			eclipse.resetSimulation();
			eclipse.resumeSimulation();
			require(advanceSeconds(eclipse, replayTicks * static_cast<double>(core::World::getFixedTimestep())),
				"Eclipse battery replay failed");
			require(eclipse.getStationSystemsSnapshot() == expected
				&& eclipse.getStationSystemsEvents() == expectedEvents,
				"Repeated eclipse battery cycles not deterministic after reset");
		}
	}

	// Split out of `batteries` so this world runs as its own ctest test rather than in one serial chain.
	void batteriesLimited(smoke::Context const&)
	{
		core::World limited("Limited", 2, 2);
		limited.pauseSimulation(); limited.finishBuild();
		require(configure(limited, {},
			{load("lamp", 3.0, 0)},
			{battery("cell", 10.0, 10.0, 10.0, 0.0, 1.0)}).first, "Power-limit scenario refused");
		limited.resumeSimulation();
		require(smoke::station::advanceSeconds(limited, 3600.0), "Power-limit run failed");
		{
			auto const snap = limited.getStationSystemsSnapshot();
			require(approx(snap.power.batteryDischargeKW, 1.0)
				&& approx(snap.power.batteryShortfallKW, 2.0)
				&& approx(snap.power.loads[0].suppliedKW, 1.0)
				&& approx(snap.power.loads[0].unmetKW, 2.0)
				&& approx(snap.power.batteries[0].storedKWh, 9.0),
				"Discharge power limit not enforced or remainder not explicit");
			require(approx(snap.power.batteries[0].deliveredKWh, 1.0)
				&& approx(snap.power.batteries[0].dischargedKWh, 1.0),
				"Lossless discharge ledger wrong");
		}
	}

	// Split out of `batteries` so this world runs as its own ctest test rather than in one serial chain.
	void batteriesLosses(smoke::Context const&)
	{
		core::World losses("Losses", 2, 2);
		losses.pauseSimulation(); losses.finishBuild();
		require(configure(losses,
			{array("field", 10.0, constantProfile(1.0))},
			{load("shop", 6.0, 0)},
			{battery("bank", 10.0, 10.0, 2.0, 2.0, 4.0, 0.5, 0.5)}).first,
			"Charge-loss scenario refused");
		losses.resumeSimulation();
		require(smoke::station::advanceSeconds(losses, 3600.0), "Charge-loss run failed");
		{
			auto const snap = losses.getStationSystemsSnapshot();
			require(approx(snap.power.batteries[0].chargedKWh, 1.0)
				&& approx(snap.power.batteries[0].chargeInputKWh, 2.0)
				&& approx(snap.power.batteries[0].storedKWh, 3.0)
				&& approx(snap.power.batteries[0].lossesKWh, 1.0)
				&& approx(snap.power.batteries[0].residualKWh, 0.0),
				"Charge efficiency loss not booked or ledger not closed");
			require(approx(snap.power.surplusKW, 2.0),
				"Surplus not reduced by the full bus power the battery consumed");
		}
	}

	// Split out of `batteries` so this world runs as its own ctest test rather than in one serial chain.
	void batteriesDischarging(smoke::Context const&)
	{
		core::World discharging("Discharging", 2, 2);
		discharging.pauseSimulation(); discharging.finishBuild();
		require(configure(discharging, {},
			{load("lamp", 1.0, 0)},
			{battery("cell", 10.0, 10.0, 10.0, 0.0, 100.0, 1.0, 0.5)}).first,
			"Discharge-loss scenario refused");
		discharging.resumeSimulation();
		require(smoke::station::advanceSeconds(discharging, 3600.0), "Discharge-loss run failed");
		{
			auto const snap = discharging.getStationSystemsSnapshot();
			require(approx(snap.power.batteries[0].dischargedKWh, 2.0)
				&& approx(snap.power.batteries[0].deliveredKWh, 1.0)
				&& approx(snap.power.batteries[0].storedKWh, 8.0)
				&& approx(snap.power.batteries[0].lossesKWh, 1.0)
				&& approx(snap.power.batteries[0].residualKWh, 0.0),
				"Discharge efficiency loss not booked or ledger not closed");
			require(snap.power.batteries[0].deliveredKWh
				<= snap.power.batteries[0].dischargedKWh + 1e-9,
				"Battery delivered more energy than it drew (free energy)");
		}
	}

	// The sub-second battery scenarios, grouped: each is a handful of ticks, so they share one test rather than adding eight more.
	void batteriesQuick(smoke::Context const&)
	{
		core::World oneSecond("SecondDischarge", 2, 2);
		oneSecond.pauseSimulation(); oneSecond.finishBuild();
		require(configure(oneSecond, {},
			{load("lamp", 1.0, 0)},
			{battery("b", 1.0, 1.0, 1.0, 1.0, 1.0)}).first,
			"One-second discharge scenario refused");
		oneSecond.resumeSimulation();
		require(smoke::station::advanceSeconds(oneSecond, 1.0), "One-second discharge run failed");
		{
			auto const snap = oneSecond.getStationSystemsSnapshot();
			auto const& b = snap.power.batteries[0];
			require(tight(b.storedKWh, 1.0 - kWs)
				&& tight(b.dischargedKWh, kWs)
				&& tight(b.deliveredKWh, kWs)
				&& tight(b.residualKWh, 0.0)
				&& tight(b.lossesKWh, 0.0)
				&& tight(snap.power.batteryDischargeKW, 1.0),
				"1 kW over one second did not move 1/3600 kWh");
		}
		core::World oneSecondCharge("SecondCharge", 2, 2);
		oneSecondCharge.pauseSimulation(); oneSecondCharge.finishBuild();
		require(configure(oneSecondCharge,
			{array("field", 1.0, constantProfile(1.0))},
			{},
			{battery("b", 2.0, 2.0, 0.0, 1.0, 1.0)}).first,
			"One-second charge scenario refused");
		oneSecondCharge.resumeSimulation();
		require(smoke::station::advanceSeconds(oneSecondCharge, 1.0), "One-second charge run failed");
		{
			auto const snap = oneSecondCharge.getStationSystemsSnapshot();
			auto const& b = snap.power.batteries[0];
			require(tight(b.storedKWh, kWs)
				&& tight(b.chargedKWh, kWs)
				&& tight(b.chargeInputKWh, kWs)
				&& tight(b.residualKWh, 0.0)
				&& tight(b.lossesKWh, 0.0)
				&& tight(snap.power.batteryChargeKW, 1.0)
				&& tight(snap.power.surplusKW, 0.0),
				"1 kW of surplus over one second did not store 1/3600 kWh");
		}
		core::World zeroed("Zeroed", 2, 2);
		zeroed.pauseSimulation(); zeroed.finishBuild();
		require(configure(zeroed, {},
			{load("lamp", 1.0, 0)},
			{battery("dead", 10.0, 0.0, 5.0, 5.0, 5.0)}).first, "Zero-capacity scenario refused");
		zeroed.resumeSimulation();
		require(smoke::station::advanceSeconds(zeroed, 1.0), "Zero-capacity run failed");
		{
			auto const snap = zeroed.getStationSystemsSnapshot();
			require(snap.power.batteries[0].disabled
				&& approx(snap.power.batteries[0].storedKWh, 5.0)
				&& approx(snap.power.batteryDischargeKW, 0.0)
				&& approx(snap.power.batteryChargeKW, 0.0),
				"Zero-usable-capacity battery not disabled or still moved energy");
			require(snap.power.loads[0].status == core::StationTransferStatus::Rejected
				&& approx(snap.power.loads[0].unmetKW, 1.0),
				"Disabled battery changed the load outcome");
		}
		core::World critical("Critical", 2, 2);
		critical.pauseSimulation(); critical.finishBuild();
		{
			core::ProfileSettings eclipseProfile;
			eclipseProfile.periodSeconds = 120.0;
			eclipseProfile.segments = {{0.0, 0.0}, {60.0, 1.0}};
			auto lifeSupport = load("lifeSupport", 8.0, 0);
			lifeSupport.criticalFloorKW = 2.0;
			require(configure(critical,
				{array("orbital", 10.0, eclipseProfile)},
				{lifeSupport, load("workshop", 8.0, 9)},
				{battery("bank", 10.0, 10.0, 10.0, 0.0, 3.0)}).first,
				"Critical-battery scenario refused");
			critical.resumeSimulation();
			// 59 s of darkness: the 3 kW discharge cap is shared, the
			// critical floor first. Energy moved is power x hours:
			// 3 kW x 59/3600 h = 0.0491667 kWh (#547).
			require(smoke::station::advanceSeconds(critical, 59.0), "Critical-battery run failed");
			{
				auto const snap = critical.getStationSystemsSnapshot();
				require(snap.power.loads[0].id == "workshop"
					&& snap.power.loads[1].id == "lifeSupport",
					"Critical-battery loads not in declared allocation order");
				require(approx(snap.power.loads[1].suppliedKW, 2.0)
					&& approx(snap.power.loads[1].unmetCriticalKW, 0.0),
					"Critical floor not protected from battery energy during eclipse");
				require(approx(snap.power.loads[0].suppliedKW, 1.0)
					&& approx(snap.power.batteryDischargeKW, 3.0),
					"Discretionary demand took energy reserved for the critical floor");
				auto const expectedStored = 10.0 - 3.0 * 59.0 / 3600.0;
				require(approx(snap.power.batteries[0].storedKWh, expectedStored),
					"Battery discharge ledger wrong under critical protection");
			}
		}
		core::World rigid("Rigid", 2, 2);
		rigid.pauseSimulation(); rigid.finishBuild();
		require(configure(rigid, {},
			{load("kiln", 10.0, 0, 0.0)},
			{battery("cell", 10.0, 10.0, 10.0, 0.0, 3.0)}).first,
			"Rigid-load battery scenario refused");
		rigid.resumeSimulation();
		require(smoke::station::advanceSeconds(rigid, 1.0), "Rigid-load run failed");
		{
			auto const snap = rigid.getStationSystemsSnapshot();
			require(snap.power.loads[0].status == core::StationTransferStatus::Rejected
				&& approx(snap.power.loads[0].suppliedKW, 0.0)
				&& approx(snap.power.batteries[0].storedKWh, 10.0)
				&& approx(snap.power.batteryDischargeKW, 0.0),
				"Battery supplied a load below its minimum operable level");
			require(sawEvent(rigid, "Load 'kiln' rejected: below the minimum operable supply level"),
				"Rigid-load refusal reason missing from the event log");
		}
		core::World reserve("Reserve", 2, 2);
		reserve.pauseSimulation(); reserve.finishBuild();
		require(configure(reserve, {},
			{load("lamp", 20.0, 0)},
			{battery("cell", 10.0, 8.0, 3.0, 0.0, 100.0)}).first,
			"Reserve-floor scenario refused");
		reserve.resumeSimulation();
		// The 1 kWh drawable slice empties at 20 kW in 180 s:
		// 20 kW x 180/3600 h = 1 kWh, landing exactly on the
		// reserve floor (#547).
		require(smoke::station::advanceSeconds(reserve, 180.166666667), "Reserve-floor run failed");
		{
			auto const snap = reserve.getStationSystemsSnapshot();
			require(approx(snap.power.batteries[0].storedKWh, 2.0)
				&& snap.power.batteries[0].atReserve
				&& snap.power.batteries[0].storedKWh >= 2.0 - 1e-9,
				"Discharge drew into the reserve floor");
			require(approx(snap.power.batteries[0].deliveredKWh, 1.0),
				"Reserve-floor delivery wrong");
		}
		// The bundled Lua model receives the battery descriptors intact
		// and mirrors the same ledger; a mismatched model is refused at
		// construction.
		SourceScope scope;
		core::setStationSystemsSourceLoader([] {
			return "return function(name, stores, transfers, conversions, solar, loads, batteries) "
				"assert(#batteries == 1); local b = batteries[1]; "
				"assert(b.id == 'bank' and b.nameplateKWh == 10 and b.usableKWh == 8); "
				"assert(b.startingStoredKWh == 8 and b.maxChargeKW == 2 and b.maxDischargeKW == 4); "
				"assert(b.chargeEfficiency == 1 and b.dischargeEfficiency == 1); "
				"return {advance=function(self, tick) return tick end} end";
		});
		core::World boundary("Boundary", 2, 2);
		boundary.pauseSimulation(); boundary.finishBuild();
		require(configure(boundary, {}, {},
			{battery("bank", 10.0, 8.0, 8.0, 2.0, 4.0)}).first,
			"Battery descriptors did not cross the Lua boundary intact");
		boundary.resumeSimulation();
		require(smoke::station::advanceSeconds(boundary, 1.0), "Battery boundary fixture run failed");
		core::setStationSystemsSourceLoader([] {
			return "return function(name, stores, transfers, conversions, solar, loads, batteries) "
				"assert(#batteries == 99, 'expected 99 batteries') "
				"return {advance=function(self, tick) return tick end} end";
		});
		core::World refusedLua("RefusedLua", 2, 2);
		refusedLua.pauseSimulation(); refusedLua.finishBuild();
		std::string diagnostic;
		require(!refusedLua.configureStationSystems({true, 1.0f, {}, {}, {}, {}, {},
				{battery("bank", 10.0, 8.0, 8.0, 2.0, 4.0)}},
				false, &diagnostic)
			&& diagnostic.find("expected 99 batteries") != std::string::npos
			&& !refusedLua.getStationSystemsSnapshot().enabled,
			"Lua battery assertion failure not contained at construction");
		core::setStationSystemsSourceLoader({});
	}

}

void registerStationBatteryChecks(std::vector<smoke::Check>& checks)
{
	checks.insert(checks.end(), {
		{"batteriesConfig", batteriesConfig},
		{"batteriesHourDischarge", batteriesHourDischarge},
		{"batteriesHourCharge", batteriesHourCharge},
		{"batteriesHourChargeLoss", batteriesHourChargeLoss},
		{"batteriesHourDischargeLoss", batteriesHourDischargeLoss},
		{"batteriesCharging", batteriesCharging},
		{"batteriesEclipse", batteriesEclipse},
		{"batteriesLimited", batteriesLimited},
		{"batteriesLosses", batteriesLosses},
		{"batteriesDischarging", batteriesDischarging},
		{"batteriesQuick", batteriesQuick}
	});
}
