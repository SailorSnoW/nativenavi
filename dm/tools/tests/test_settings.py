"""Offer the resolution control only when the rewritten build supports it, and the upscaler choice only to games whose shader map holds their own upscaler (game.py upscaler_keys)."""
from pathlib import Path
import sys,tempfile,unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'games'))
import settings
OOT='0400486024DEC000'

class ResolutionSetting(unittest.TestCase):
    def test_verified_rewrite_required(self):
        with tempfile.TemporaryDirectory() as tmp,patch.object(settings.nacp,'languages',return_value=['en-US']):
            p=Path(tmp)/'main.fixups.tsv'
            for header,supported in [('# BUILD_ID\t3c10ec35c5ef4c3686b4053afb0e58e7\n',False),
                                     ('# DYNAMIC_RESOLUTION\t1\n',True),('',False)]:
                p.write_text(header)
                rows=settings.build(OOT,tmp,'unused')['settings']
                dr=[s for s in rows if s['env']=='DM_DYNAMIC_RESOLUTION']
                self.assertEqual(bool(dr),supported)
                if dr:self.assertEqual((dr[0]['kind'],dr[0]['default']),('toggle','0'))
                fps=next(s for s in rows if s['env']=='DM_FPS_TARGET')
                self.assertEqual(fps['default'],'60')
                self.assertEqual(fps['options'],[['30','30 fps'],['60','60 fps']])

class FrameRateOption(unittest.TestCase):
    def test_no_120_for_a_game_not_checked_at_that_speed(self):
        with tempfile.TemporaryDirectory() as tmp,patch.object(settings.nacp,'languages',return_value=['en-US']):
            (Path(tmp)/'main.fixups.tsv').write_text('')
            fps=next(s for s in settings.build(OOT,tmp,'unused')['settings'] if s['env']=='DM_FPS_TARGET')
            self.assertEqual(fps['default'],'60')
            self.assertEqual(fps['options'],[['30','30 fps'],['60','60 fps']])

class UpscalerSetting(unittest.TestCase):
    def test_offered_to_a_game_naming_its_upscaler(self):
        with tempfile.TemporaryDirectory() as tmp,patch.object(settings.nacp,'languages',return_value=['en-US']):
            (Path(tmp)/'main.fixups.tsv').write_text('')
            up=[s for s in settings.build(OOT,tmp,'unused')['settings'] if s['env']=='DM_UPSCALER']
            self.assertTrue(up)
            self.assertEqual((up[0]['default'],up[0]['options']),('game',[['game',"Game's own"],['metalfx','MetalFX']]))

if __name__=='__main__':unittest.main()
