# #764 coarse-step overshoot measurement spike

## Method

This was a local-only measurement against the current sampled-protection implementation. A temporary branch of `scenarioBundled` constructed the existing `power-cooling-outage` configuration at Station step ratios 1, 6, and 30, drove the Station runtime directly for 900 simulated seconds, and recorded every committed sample of its declared thermal, atmosphere, and chiller limits. The temporary harness was reverted before this report was added; no executable, test, or runtime code is part of this ticket.

The scenario contains the intended coincidence: a 120--420 s solar eclipse and a simultaneous 25% `chillerMain` derate. Its declared thermal nodes are `chilledWater` (20,000 kJ/K), `processSink` (50,000 kJ/K), and `genSink` (20,000 kJ/K). The two 20,000 kJ/K nodes are the smallest thermal masses. `chilledWater` is the limiting failure node because the derated chiller coincides with the cooling outage; `genSink` sees the generator startup/shutdown heat but has substantially more limit margin.

For each thermal node, the reported open-loop figure is the final sampled `abs(lastNetKW) * stepSeconds / heatCapacityKJPerK`: the one-step temperature excursion implied by the current rate. It is a diagnostic comparison, not a protection proof; the current implementation samples and reports limit crossings rather than predicting them.

## Results

All values are extrema over 900 simulated seconds. No declared limit was crossed at any ratio, so measured overshoot is **0** for every row.

| Limit | Ratio 1 | Ratio 6 | Ratio 30 | Limit | Terminal open-loop step estimate at ratio 30 |
| --- | ---: | ---: | ---: | ---: | ---: |
| `chilledWater` temperature maximum | 8.101164636 C | 8.101167535 C | 8.101181450 C | 25 C | 0.000046575 K |
| `processSink` temperature maximum | 25.014613187 C | 25.014613188 C | 25.014613190 C | 95 C | 0.000012176 K |
| `genSink` temperature maximum | 20.411415013 C | 20.411363465 C | 20.411114218 C | 120 C | 0.000002257 K |
| `hab` pressure range | 118.794335079--121.009786186 kPa | 118.794335080--121.009759117 kPa | 118.794335089--121.009629186 kPa | 80--140 kPa | n/a |
| `hab` O2 partial-pressure minimum | 22.642867205 kPa | 22.642867182 kPa | 22.642867069 kPa | 12 kPa | n/a |
| `hab` CO2 partial-pressure maximum | 0.278804931 kPa | 0.278804634 kPa | 0.278803209 kPa | 0.4 kPa | n/a |
| `greenhouse` pressure range | 112.464918145--112.488910168 kPa | 112.464920388--112.488910244 kPa | 112.464931154--112.488910245 kPa | 80--140 kPa | n/a |
| `greenhouse` O2 / CO2 extrema | 23.163142955 / 0.421041117 kPa | 23.163144314 / 0.421039959 kPa | 23.163150837 / 0.421034399 kPa | >=12 / <=5 kPa | n/a |
| `pond` pressure range | 114.235508403--114.268055900 kPa | 114.235511418--114.268055906 kPa | 114.235525887--114.268055970 kPa | 80--140 kPa | n/a |
| `pond` O2 / CO2 extrema | 23.241094300 / 1.126554815 kPa | 23.241097334 / 1.126551808 kPa | 23.241111899 / 1.126537375 kPa | >=12 / <=8 kPa | n/a |
| `chillerMain` condenser-sink maximum | 25.014613187 C | 25.014613188 C | 25.014613190 C | 45 C | n/a |

The largest observed coarse/fine difference was the `chilledWater` peak: +0.000002899 K at ratio 6 and +0.000016814 K at ratio 30. This is smaller than both the one-step open-loop estimate and the scenario's 16.9 K margin to its declared upper limit. There is no visible control ringing in this workload.

## Recommendation

Ratio 6 is promising for this representative eclipse-and-cooling failure: it introduced no observed limit overshoot, and ratio 30 remained stable with the same result. It is **not certifiable with current protection**, however. This configuration never drives a protected limit near a trip boundary, and the current sampled protection has no pre-step rate bound. A zero overshoot here therefore does not establish a general bound for the required smallest-mass/largest-heat/worst-coincidence case.

Rate-aware protection remains required before a certified coarse ratio can be claimed. After it lands, repeat this measurement with deliberately margin-tight thermal and atmosphere fixtures that force each protection path; ratio 6 is the first plausible certification target. Ratio 30 is an informative performance/stability probe only, not an achievable certified claim on the evidence here.

## Reproduction evidence

- Temporary harness command: `cmake --build build-linux --config Release --target pf-smoke-station -j 16 && build-linux/bin/x64/Release/pf-smoke-station --check scenariosBundled`
- The temporary source patch and generated output were discarded after capture.
- The unmodified Release `station-scenariosBundled` check passed after the measurement harness ran.
