import copy
import unittest
from generate import compile_catalog


class CatalogTests(unittest.TestCase):
    def test_preserves_large_ids_and_state_identity(self):
        row=dict(stateId='8',pointId='1477783180906614784',sources=[dict(bvid='BVtest',part=1,eventId='BVtest:p1:e001')],reviewBasis=['user'])
        catalog=dict(schemaVersion=1,points=[row,dict(row,stateId='909')])
        valid={(r['stateId'],r['pointId']) for r in catalog['points']}
        self.assertEqual({'8':[row['pointId']],'909':[row['pointId']]},compile_catalog(catalog,valid)['states'])
        for case in ('duplicate','number','missing','unreviewed','wrong-source','empty-basis','bad-version'):
            c=copy.deepcopy(catalog)
            if case=='duplicate':c['points'].append(row)
            if case=='number':c['points'][0]['pointId']=int(row['pointId'])
            if case=='missing':c['points'][0]['stateId']='7'
            if case=='unreviewed':c['points'][0]['sources']=[]
            if case=='wrong-source':c['points'][0]['sources'][0]['eventId']='BVother:p2:e001'
            if case=='empty-basis':c['points'][0]['reviewBasis']=['']
            if case=='bad-version':c['schemaVersion']=True
            with self.assertRaises(ValueError):compile_catalog(c,valid)
