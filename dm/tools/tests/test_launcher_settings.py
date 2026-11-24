"""The launcher's settings (dm/games/settings.tsv through dm/games/settings.py): rows by profile, single-screen sections, toggle values and the strict format;
the meta column's optional fields, and the written list against docs/launcher.schema.json, the contract front ends read."""
import importlib.util
import json
import os
import re
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('launcher_settings', ROOT / 'dm/games/settings.py')
assert spec and spec.loader
settings = importlib.util.module_from_spec(spec)
spec.loader.exec_module(settings)

OOT = '0400486024DEC000'
PERFORMANCE_NVN = ['DM_PRESENT_SNAP', 'DM_PSO_MTL4', 'DM_PSO_PREBUILD', 'DM_NVN_ENC_THREAD', 'DM_FPS_HOLD', 'DM_METAL_HUD']


def build(gid):
    with tempfile.TemporaryDirectory() as tmp, patch.object(settings.nacp, 'languages', return_value=['en-US']):
        return {s['env']: s for s in settings.build(gid, tmp, Path(tmp) / 'control.nacp')['settings']}


def rows_with_languages(listed):   # the settings rows, by variable, of a game whose control data lists these languages
    with tempfile.TemporaryDirectory() as tmp, patch.object(settings.nacp, 'languages', return_value=listed), patch.dict(os.environ, {'DM_LANG': 'en-US'}):
        return {s['env']: s for s in settings.build(OOT, tmp, Path(tmp) / 'control.nacp')['settings']}


class LauncherSettingsTests(unittest.TestCase):
    def test_game_language_row_offers_the_games_own_or_every_language_when_unknown(self):
        self.assertNotIn('DM_LANG', rows_with_languages(['ja']))   # one language: nothing to pick
        s = rows_with_languages(['en-US', 'ja'])['DM_LANG']
        self.assertEqual((s['options'], s['note']), ([['en-US', 'American English'], ['ja', 'Japanese']], ''))
        s = rows_with_languages([])['DM_LANG']   # no control data: a dump, a NUP without a NACP
        self.assertEqual(s['options'], [list(l) for l in settings.nacp.LANGUAGES])
        self.assertEqual((s['default'], s['note']), ('en-US', settings.UNKNOWN_LANGUAGES))

    def test_upscaling_is_opt_in(self):
        s = build(OOT)['DM_PRESENT_UPSCALE']
        self.assertEqual((s['kind'], s['section'], s['default']), ('choice', 'Game', 'off'))
        self.assertEqual(s['options'], [['off', 'Standard'], ['metalfx', 'MetalFX']])

    def test_frame_buffering_follows_the_profile(self):
        s = build(OOT)['DM_PRESENT_SNAP']
        self.assertEqual((s['kind'], s['section'], s['default']), ('toggle', 'Performance', '0'))   # the profile has no present_snap
        self.assertIn('latency', s['note'])

    def test_sections_keep_the_host_defaults(self):
        oot = build(OOT)
        self.assertEqual([e for e, s in oot.items() if s['section'] == 'Performance'], PERFORMANCE_NVN)
        # nvn_archive.inc pso4_on: on; nvn_rec.h rec_on: off unless the profile says enc_thread on, as this game's does
        self.assertEqual((oot['DM_PSO_MTL4']['default'], oot['DM_PSO_PREBUILD']['default'], oot['DM_NVN_ENC_THREAD']['default'], oot['DM_METAL_HUD']['default']), ('1', '1', '1', '0'))
        self.assertEqual(list(dict.fromkeys(s['section'] for s in oot.values())), ['Game', 'Controls', 'Performance'])
        self.assertIn('DM_FPS_HOLD', oot)   # a frame step (fps_target), as the Frame rate row
        self.assertIn('first start', oot['DM_PSO_PREBUILD']['note'])   # it runs once in the prepare step, before the first frame

    def test_hold_frames_takes_on_and_off(self):   # the host reads DM_FPS_HOLD as on / off (hle_nvn.c), not 1 / 0
        s = build(OOT)['DM_FPS_HOLD']
        self.assertEqual((s['on'], s['off'], s['default']), ('on', 'off', 'on'))
        s = build(OOT)['DM_RUMBLE']
        self.assertEqual((s['on'], s['off']), ('1', '0'))

    def test_the_mac_front_ends_rows(self):   # the window's Display mode (front_mac.m reads 1 or 0); Switch 2 controllers (pad_switch2_apple.m)
        mac = build(OOT)
        self.assertFalse({'DM_SHOW_FPS', 'DM_TOUCH_HAPTICS', 'DM_PICTURE_FIT', 'DM_SCREEN_ASPECT'} & set(mac))   # no other front end's switches
        self.assertTrue({'DM_RUMBLE', 'DM_METAL_HUD', 'DM_FULLSCREEN', 'DM_SW2'} <= set(mac))
        s = mac['DM_FULLSCREEN']
        self.assertEqual((s['kind'], s['default'], s['options'], s['level']), ('choice', '0', [['0', 'Window'], ['1', 'Fullscreen']], 'basic'))

    def test_practice_tools_only_in_an_app_that_carries_them(self):   # the packing script's practice_tools folder, which pack_app.py's --practice puts in Resources/dm/practice
        self.assertNotIn('DM_PRACTICE', build(OOT))   # no practice/: a build with --no-practice, or a release without a patch for main
        with tempfile.TemporaryDirectory() as tmp, patch.object(settings.nacp, 'languages', return_value=['en-US']):
            tools = Path(tmp) / settings.PRACTICE_DIR; tools.mkdir()
            (tools / 'subsdk9.patched.bin').write_bytes(b'code')
            self.assertNotIn('DM_PRACTICE', [s['env'] for s in settings.build(OOT, tmp, Path(tmp) / 'control.nacp')['settings']])   # a module without its IPS
            (tools / 'E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348.ips').write_bytes(b'PATCH')
            listed = settings.build(OOT, tmp, Path(tmp) / 'control.nacp')['settings']
        s = {r['env']: r for r in listed}['DM_PRACTICE']
        self.assertEqual((s['kind'], s['section'], s['default'], s['on'], s['off'], s['level']), ('toggle', 'Practice', '0', '1', '0', 'basic'))   # hle_practice.c loads them on 1 only
        self.assertIn('right stick down', s['note'])   # how the menu opens (sloperinaGZ's USAGE.md)
        self.assertEqual(list(dict.fromkeys(r['section'] for r in listed)), ['Game', 'Controls', 'Performance', 'Practice'])

    def test_written_list_follows_the_schema(self):
        schema = json.loads((ROOT / 'docs/launcher.schema.json').read_text(encoding='utf-8'))
        with tempfile.TemporaryDirectory() as tmp, patch.object(settings.nacp, 'languages', return_value=['en-US']):
            self.assertEqual(conforms(settings.build(OOT, tmp, Path(tmp) / 'control.nacp'), schema, schema), [])

    def test_meta_fields_and_the_schema_version(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(settings.nacp, 'languages', return_value=['en-US', 'ja']):
            listed = settings.build(OOT, tmp, Path(tmp) / 'control.nacp')
        rows = {s['env']: s for s in listed['settings']}
        self.assertEqual(listed['schema'], 1)
        self.assertEqual([e for e, s in rows.items() if s.get('level') == 'basic'], ['DM_FPS_TARGET', 'DM_OPMODE', 'DM_LANG', 'DM_FULLSCREEN', 'DM_RUMBLE'])
        self.assertEqual(rows['DM_RUMBLE_GAIN']['needs'], 'DM_RUMBLE=1')
        self.assertFalse({'level', 'needs'} & set(rows['DM_METAL_HUD']), 'a row without meta has no optional fields')

    def test_meta_is_strict(self):
        self.assertEqual(settings.meta('level=basic; needs=DM_RUMBLE=1', 1), {'level': 'basic', 'needs': 'DM_RUMBLE=1'})
        self.assertEqual(settings.meta('', 1), {})
        for bad, why in [('colour=red', 'unknown meta key'), ('level', 'unknown meta key'), ('level=basic;level=basic', 'twice'),
                         ('level=advanced', 'does not match'), ('needs=DM_RUMBLE', 'does not match'), ('needs=rumble=1', 'does not match')]:
            with self.subTest(bad=bad), self.assertRaises(SystemExit) as e:
                settings.meta(bad, 1)
            self.assertIn(why, str(e.exception))

    def test_meta_keys_agree_with_the_docs_and_the_schema(self):
        """A key is added in settings.py's META, docs/frontends.md and docs/launcher.schema.json together"""
        schema = json.loads((ROOT / 'docs/launcher.schema.json').read_text(encoding='utf-8'))
        fields = schema['$defs']['setting']['properties']
        docs = (ROOT / 'docs/frontends.md').read_text(encoding='utf-8')
        for key in settings.META:
            with self.subTest(key=key):
                self.assertIn(key, fields)
                self.assertNotIn(key, schema['$defs']['setting']['required'], 'a meta field is optional')
                self.assertRegex(docs, rf'- `{key}` \(optional\)')
        self.assertEqual(schema['properties']['schema']['const'], settings.SCHEMA)

    def test_schema_1_takes_new_sections_but_not_new_kinds(self):
        """A section is a heading a reader draws by its name, so a new one stays schema 1; a new kind would not"""
        schema = json.loads((ROOT / 'docs/launcher.schema.json').read_text(encoding='utf-8'))
        row = {'env': 'DM_X', 'kind': 'toggle', 'section': 'Picture', 'label': 'X', 'note': '', 'default': '1', 'on': '1', 'off': '0'}
        self.assertEqual(conforms({'schema': 1, 'game': OOT, 'settings': [row]}, schema, schema), [])
        self.assertNotEqual(conforms({'schema': 1, 'game': OOT, 'settings': [{**row, 'kind': 'slider'}]}, schema, schema), [])

    def test_table_format_is_strict(self):
        good = 'DM_X\ttoggle\t-\t0\talways\tGame\tX\n'
        for bad, why in [('DM_X\ttoggle\t-\t0\talways\tGame\tX\t\tlevel=basic\textra\n', 'columns'),
                         ('DM_X\ttoggle\t-\t0\talways\tGame\tX\t\tneeds=DM_NOPE=1\n', 'no row sets'),
                         ('DM_Y\ttoggle\t-\t0\talways\tGame\tY\t\tneeds=DM_X=on\n', 'never takes'),
                         ('DM_X\ttoggle\t-\t0\talways\tX\n', 'columns'), ('DM_X\ttoggle\t-\t0\talways\tsecret\tX\n', 'section'),
                         ('DM_X\ttoggle\ton\t0\talways\tGame\tX\n', 'toggle values'), ('DM_X\ttoggle\t-\t2\talways\tGame\tX\n', 'defaults to 0 or 1')]:
            with self.subTest(why=why), tempfile.TemporaryDirectory() as tmp:
                (Path(tmp) / 'settings.tsv').write_text('# header\n' + good + bad, encoding='utf-8')
                with patch.object(settings, 'HERE', Path(tmp)), \
                        patch.object(settings.nacp, 'languages', return_value=['en-US']), self.assertRaises(SystemExit) as e:
                    settings.build(OOT, tmp, Path(tmp) / 'control.nacp')
                self.assertIn(why, str(e.exception))


def conforms(value, node, root, at='$'):
    """The JSON Schema keywords launcher.schema.json uses (type, required, properties, items, enum, const, pattern, $ref, allOf with
    if / then), as a list of problems: enough to hold the written lists to the contract without a jsonschema package"""
    if '$ref' in node: return conforms(value, root['$defs'][node['$ref'].rsplit('/', 1)[1]], root, at)
    problems = []
    kinds = {'object': dict, 'array': list, 'string': str, 'integer': int, 'number': (int, float)}
    if 'type' in node and not isinstance(value, kinds[node['type']]): return [f'{at}: not a {node["type"]}']
    if 'const' in node and value != node['const']: problems.append(f'{at}: not {node["const"]!r}')
    if 'enum' in node and value not in node['enum']: problems.append(f'{at}: not one of {node["enum"]}')
    if 'pattern' in node and not re.search(node['pattern'], value): problems.append(f'{at}: does not match {node["pattern"]}')
    if isinstance(value, dict):
        problems += [f'{at}: no {k}' for k in node.get('required', []) if k not in value]
        for k, v in value.items():
            if k in node.get('properties', {}): problems += conforms(v, node['properties'][k], root, f'{at}.{k}')
        for rule in node.get('allOf', []):
            if not conforms(value, rule['if'], root, at): problems += conforms(value, rule['then'], root, at)
    if isinstance(value, list) and 'items' in node:
        for i, v in enumerate(value): problems += conforms(v, node['items'], root, f'{at}[{i}]')
    return problems


if __name__ == '__main__':
    unittest.main()
