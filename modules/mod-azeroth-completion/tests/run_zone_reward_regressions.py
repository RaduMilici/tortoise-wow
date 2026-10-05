#!/usr/bin/env python3
"""Check reward generation and retirement without a server or database."""
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
MODULE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(MODULE / 'tools/zone_rewards'))
import build


class ZoneRewardTests(unittest.TestCase):
    def test_checked_in_outputs_are_current(self):
        zones = [build.Zone(i, z) for i, z in enumerate(build.ZONES)]
        build.check(zones)
        self.assertEqual(Path(build.SQL_OUT).read_text(), build.build_sql(zones))
        self.assertEqual(json.loads(Path(build.JSON_OUT).read_text()), build.build_client(zones))

    def test_retirement_preserves_owned_rewards_through_cli(self):
        definitions = [dict(build.ZONES[0], retired=True), build.ZONES[1]]
        zones = [build.Zone(i, z) for i, z in enumerate(definitions)]
        with tempfile.TemporaryDirectory(prefix='azc-zone-rewards-') as directory:
            sql, client = Path(directory) / 'world.sql', Path(directory) / 'spells.json'
            with patch.object(build, 'ZONES', definitions), \
                    patch.object(build, 'SQL_OUT', str(sql)), \
                    patch.object(build, 'JSON_OUT', str(client)), \
                    patch.object(sys, 'argv', ['build.py', '--addon', directory]), \
                    contextlib.redirect_stdout(io.StringIO()):
                build.main()
            generated = sql.read_text()
            # Every definition still exists, including the retired zone's spell IDs.
            self.assertEqual(json.loads(client.read_text()), build.build_client(zones))
            self.assertIn('PVP_MEDAL70 = "Defender of Elwynn"',
                          (Path(directory) / 'ZoneTitles.lua').read_text())
            self.assertIn('`entry` = 93500', generated)
            self.assertIn('`entry` = 94000', generated)
            self.assertIn('VALUES (93504, 64002)', generated)
            # The surviving zone keeps its original IDs; only its rewards are offered.
            rewards = generated.split('INSERT INTO `azcomp_milestone_reward`', 1)[1]
            self.assertNotIn('(100000, 12,', rewards)
            self.assertIn('(100010, 40, 25,', rewards)

    def test_all_retired_does_not_emit_empty_insert(self):
        zone = build.Zone(0, dict(build.ZONES[0], retired=True))
        sql = build.build_sql([zone])
        self.assertNotIn('INSERT INTO `azcomp_milestone_reward`', sql)
        self.assertIn('DELETE FROM `azcomp_milestone_reward`', sql)
        self.assertIn('`entry` = 64002', sql)

    def test_duplicate_or_generic_area_is_rejected(self):
        for area in (0, -1, build.ZONES[0]['area']):
            with self.subTest(area=area), self.assertRaises(SystemExit):
                build.check([build.Zone(0, build.ZONES[0]),
                             build.Zone(1, dict(build.ZONES[1], area=area))])

    def test_retired_set_can_be_replaced_in_same_area(self):
        old = build.Zone(0, dict(build.ZONES[0], retired=True))
        new = build.Zone(1, dict(build.ZONES[1], area=build.ZONES[0]['area']))
        build.check([old, new])
        rewards = build.build_sql([old, new]).split('INSERT INTO `azcomp_milestone_reward`', 1)[1]
        self.assertNotIn('(100000,', rewards)
        self.assertIn(f"(100010, {build.ZONES[0]['area']}, 25,", rewards)

    def test_actual_indices_are_validated(self):
        for index in (-1, 0, build.MAX_ZONES):
            with self.subTest(index=index), self.assertRaises(SystemExit):
                build.check([build.Zone(0, build.ZONES[0]),
                             build.Zone(index, build.ZONES[1])])


if __name__ == '__main__':
    unittest.main()
