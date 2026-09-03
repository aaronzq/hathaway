import json
import unittest
from pathlib import Path


HERE = Path(__file__).resolve().parent
DASHBOARD = HERE / "grafana/provisioning/dashboards/hathaway_review.json"


class HathawayReviewDashboardTest(unittest.TestCase):
    def setUp(self):
        self.dashboard = json.loads(DASHBOARD.read_text())

    def panel(self, title):
        for panel in self.dashboard["panels"]:
            if panel["title"] == title:
                return panel
        self.fail(f"missing panel {title!r}")

    def variable(self, name):
        for variable in self.dashboard["templating"]["list"]:
            if variable["name"] == name:
                return variable
        self.fail(f"missing variable {name!r}")

    def test_trial_outcomes_include_teach(self):
        raster_sql = "\n".join(
            target["rawSql"]
            for target in self.panel("Lick, Reward & Outcome events")["targets"]
        )
        pie = self.panel("Trial outcomes  --  $total_trials trials in session $session")
        pie_sql = pie["targets"][0]["rawSql"]
        pie_overrides = pie["fieldConfig"]["overrides"]

        self.assertIn("channel = 4", raster_sql)
        self.assertIn("AS teach", raster_sql)
        self.assertIn("channel = 4", pie_sql)
        self.assertIn('AS "teach"', pie_sql)
        self.assertTrue(
            any(override["matcher"]["options"] == "teach" for override in pie_overrides)
        )

    def test_summary_panels_are_gated_to_selected_time_range(self):
        titles = [
            "Cumulative rewards (selected session)",
            "Trial outcomes  --  $total_trials trials in session $session",
            "Trials per hour (selected session)",
        ]
        for title in titles:
            sql = self.panel(title)["targets"][0]["rawSql"]
            with self.subTest(panel=title):
                self.assertIn("session_id = $session", sql)
                self.assertIn("$__timeFilter", sql)

    def test_total_trials_matches_selected_time_range(self):
        total_trials = self.variable("total_trials")

        self.assertIn("$__timeFilter", total_trials["query"])
        self.assertEqual(total_trials["refresh"], 2)


if __name__ == "__main__":
    unittest.main()
