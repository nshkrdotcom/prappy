"""One-time conversion keeps user edits, history IDs and playback positions."""
from copy import deepcopy
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from migrate_native_state import Clock, migrate_workspace, migrate_history


class NativeStateMigration(unittest.TestCase):
    def fixture(self):
        library='lib_'+'1'*32; capture='cap_'+'2'*32
        old_timeline='tl_'+'3'*32; new_timeline='tl_'+'4'*32; region='reg_'+'5'*32
        old={'library_id':library,'capture_id':capture,'timeline_id':old_timeline}
        native={'id':new_timeline,'capture_id':capture,'sample_rate':44100,
            'channels':1,'frame_count':44100,'working_asset_key':'a'*64,'sample_format':'float32'}
        prepared={'state':'Ready','key':'a'*64,'source_sha256':'b'*64,
            'capture':{'id':capture,'timeline_id':new_timeline,'timeline':native,'blob_sha256':'b'*64}}
        bundle={'schema_version':1,'windows_state_directory':r'C:\player',
            'sources':[{'library_id':library,'timeline':{'id':old_timeline,'capture_id':capture,
                'sample_rate':48000,'frame_count':48000},'prepared':prepared}],
            'region_revisions':{library+'/'+region+':1':{'id':region,'revision':2}}}
        attributes={'name':'Keep this edit','fields':{'notes':'日本語'},'metadata_target':region,
            'metadata_target_revision':1,'metadata_revision':1,
            'metadata':{'target_id':region,'target_revision':1,'revision':1,'effective':{'notes':'日本語'}}}
        draft={'id':'my-draft','region':{'id':region,'revision':1},'saving':None,
            'value':{'start':12000,'end':24000,'attributes':attributes},
            'baseline':{'start':12000,'end':22000,'attributes':deepcopy(attributes)}}
        row={'source':old,'total':48000,'active':'my-draft','drafts':[draft],
            'view':{'capture':{},'cursor':24000,'viewport_start':12345.5,'viewport_end':48000,
                'amplitude_gain':1.2,'loop':True}}
        state={'workspaces':{'sources':[row]},'active_source':old,'commands':[],
            'open':True,'source_handoff':None,'source_handoff_digest':'old-digest',
            'source_handoff_library':library,'source_handoff_request':None,
            'browser_library':library,'search':'song','facet':'','facet_value':'',
            'tab':1,'offset':0,'gain':-2.0}
        return state,bundle

    def test_workspace_preserves_drafts_baselines_metadata_and_view(self):
        state,bundle=self.fixture(); original=deepcopy(state)
        converted=migrate_workspace(state,bundle)
        self.assertEqual(state,original)
        row=converted['workspaces']['sources'][0]
        self.assertEqual(row['total'],44100)
        self.assertEqual(row['source']['timeline_id'],'tl_'+'4'*32)
        value=row['drafts'][0]['value']; baseline=row['drafts'][0]['baseline']
        self.assertEqual((value['start'],value['end']),(11025,22050))
        self.assertEqual(baseline['end'],20213)
        self.assertNotEqual(value,baseline)
        self.assertEqual(value['attributes']['fields']['notes'],'日本語')
        self.assertEqual(value['attributes']['metadata_target_revision'],2)
        self.assertEqual(value['attributes']['metadata']['target_revision'],2)
        self.assertEqual(row['drafts'][0]['region']['revision'],2)
        self.assertEqual(row['view']['cursor'],22050)
        self.assertAlmostEqual(row['view']['viewport_start'],12345.5*44100/48000)
        self.assertEqual(row['view']['viewport_end'],44100)
        self.assertEqual(row['view']['amplitude_gain'],1.2)
        self.assertEqual(converted['prepared_manifest'],bundle['sources'][0]['prepared'])
        self.assertNotIn('source_handoff_digest',converted)
        self.assertIsNone(converted['source_handoff_source'])

    def test_pending_commands_are_never_silently_discarded(self):
        state,bundle=self.fixture(); state['commands']=[{'request_id':'still-pending'}]
        with self.assertRaisesRegex(ValueError,'pending'):migrate_workspace(state,bundle)
        self.assertEqual(state['commands'][0]['request_id'],'still-pending')

    def test_native_frames_remain_exact_beyond_32_bits(self):
        clock=Clock({'sample_rate':48000,'frame_count':1555200001},
            {'sample_rate':192000,'frame_count':6220800004})
        self.assertEqual(clock.frame(1440000000),5760000000)
        self.assertEqual(clock.frame(1555200001),6220800004)
        with self.assertRaises(ValueError):clock.frame(True)

    def test_history_preserves_ids_queue_order_and_exact_seconds(self):
        path='\\\\wsl.localhost\\ubuntu26-dev\\music\\日本語 "song".opus'
        original='PRAPPY_HISTORY 1\nNEXT_ID 3\nREPEAT 2\nENTRY 1 123 1.123456789 '+json.dumps(path,ensure_ascii=False)+\
            '\nQUEUED 2 0 0 '+json.dumps(path,ensure_ascii=False)+'\nTRAIL 1 2\nCURSOR 1\n'
        ref={'library_id':'lib_'+'1'*32,'capture_id':'cap_'+'2'*32}
        migrated=migrate_history(original,{path:ref},r'C:\player')
        expected=json.dumps('C:\\player\\sources\\'+ref['library_id']+'\\'+ref['capture_id']+'.source')
        self.assertIn('ENTRY 1 123 1.123456789 '+expected,migrated)
        self.assertIn('QUEUED 2 0 0 '+expected,migrated)
        self.assertTrue(migrated.endswith('TRAIL 1 2\nCURSOR 1\n'))
        self.assertIn('REPEAT 2',migrated)
        with self.assertRaisesRegex(ValueError,'unmapped'):migrate_history(original,{},r'C:\player')


if __name__=='__main__':unittest.main()
