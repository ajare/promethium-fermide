#!/usr/bin/env python3
"""Regression checks for the snapshot coverage inventory's header parser."""
import unittest

from station_observable_registry import snapshot_members


class SnapshotMembersTests(unittest.TestCase):
    def test_forward_declaration_before_unrelated_struct(self):
        self.assertEqual(snapshot_members("""
            struct StationSystemsSnapshot;
            struct StationMonitorMetric { std::string_view name, unit; };
            struct StationSystemsSnapshotExtra { bool unrelated; };
            struct StationSystemsSnapshot
            {
                std::vector<StationAlertSnapshot> activeAlerts, closedAlerts;
                bool enabled{false};
                std::array<uint64_t, 4> completedTicks{};
                bool operator==(const StationSystemsSnapshot&) const = default;
            };
            struct Following { bool excluded; };
        """), {"activeAlerts", "closedAlerts", "enabled", "completedTicks"})

    def test_comments_and_definition_whitespace(self):
        self.assertEqual(snapshot_members("""
            // struct StationSystemsSnapshot { bool fake; };
            /* struct StationSystemsSnapshot { bool fakeToo; }; */
            struct\tStationSystemsSnapshot /* comment */ { double elapsedSeconds{0}; };
        """), {"elapsedSeconds"})

    def test_forward_declaration_is_not_a_definition(self):
        with self.assertRaisesRegex(SystemExit, "Snapshot definition not found"):
            snapshot_members("""
                struct StationSystemsSnapshot;
                struct Other { bool unrelated; };
            """)

    def test_unrecognized_member_is_not_silently_ignored(self):
        with self.assertRaisesRegex(SystemExit, "Unrecognized snapshot declaration"):
            snapshot_members("struct StationSystemsSnapshot { double *unsupported; };")


if __name__ == "__main__":
    unittest.main()
