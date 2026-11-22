
from pathlib import Path
import csv,json,re,statistics as stats,sys
root=Path(sys.argv[1])
old=Path(sys.argv[2])
pattern=re.compile(r'^(Exp\S+)\s+\|\s+(\S+)\s+\|\s+(\S+)\s+\|\s+ok=(-?\d+)\s+\|\s+FCT=\s*(\d+\.\d+) ms.*?\bdrop=\s*(\d+).*?\bretx=\s*(\d+).*?g=\[([\d.]+)\.\.([\d.]+)\].*?CV500=([\d.]+)\s+\|\s+CVmid=([\d.]+)', re.M)
keys=['scenario','mode','cc','ok','fct_ms','drop','retx','g_min','g_max','cv500','cvmid']
def deterministic(p):
    return [dict(zip(keys,m.groups())) for m in pattern.finditer(p.read_text())]
def read(p):
    with p.open(newline='') as f: return list(csv.DictReader(f))
det=deterministic(root/'run.log')
multi=read(root/'picoquic_multiseed_runs.csv')
assert (root/'exit-code.txt').read_text().strip()=='0'
assert len(det)==63 and len(multi)==280 and all(r['ok']=='0' for r in det+multi)
old_multi={(r['scenario'],r['seed'],r['mode']):r for r in read(old/'picoquic_multiseed_runs.csv')}
old_det={(r['scenario'],r['mode']):r for r in deterministic(old/'picoquic_all_results.txt')}
def diff(a,b):
    return any(a[k]!=b[k] for k in ('ok','fct_ms','drop','retx','cv500','cvmid'))
multi_diff=[(r['scenario'],r['seed'],r['mode']) for r in multi if r['mode']!='BudgetO1' and diff(r,old_multi[(r['scenario'],r['seed'],r['mode'])])]
det_diff=[(r['scenario'],r['mode']) for r in det if r['mode']!='BudgetO1' and diff(r,old_det[(r['scenario'],r['mode'])])]
medians=[]
for sc in sorted({r['scenario'] for r in multi}):
    for mode in sorted({r['mode'] for r in multi}):
        group=[r for r in multi if r['scenario']==sc and r['mode']==mode]
        assert len(group)==20
        medians.append(dict(scenario=sc,mode=mode,n=len(group),**{k:stats.median(float(r[k]) for r in group) for k in ('fct_ms','cv500','drop','retx')}))
with (root/'deterministic.csv').open('w',newline='') as f:
    w=csv.DictWriter(f,fieldnames=keys); w.writeheader(); w.writerows(det)
res={'deterministic_passed':len(det),'multiseed_passed':len(multi),
    'native_and_fixed_deterministic_diff':det_diff,'native_and_fixed_multiseed_diff':multi_diff,'medians':medians}
(root/'summary.json').write_text(json.dumps(res,indent=2)+'\n')
print(json.dumps(res,indent=2))

