# Answer table for translation queries (the profile's shader_facts.tsv): per shader key, the facts the machine code does not hold; other keys get defaults.
# Usage: dmsh_facts.py   prints the row count and each fact's coverage
import json, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '../../games'))
import game as _game
TABLE = str(_game.load().table('shader_facts.tsv'))
def load():
    """key → fact dict"""
    if not os.path.exists(TABLE): return {}
    return {k: json.loads(v) for k, v in (l.rstrip('\n').split('\t', 1) for l in open(TABLE) if l.strip() and not l.startswith('#'))}

def row(key, facts): return key + '\t' + json.dumps(facts, sort_keys=True, separators=(',', ':'), ensure_ascii=False) + '\n'

if __name__ == '__main__':
    a = sys.argv[1:]
    if not a:
        t = load()
        print(f'{len(t)} keys: ' + ', '.join(f'{n} {sum(bool(f.get(n)) for f in t.values())}' for n in ('tex', 'img', 'attr', 'fout', 'local', 'shared')))
    else: sys.exit('usage: see the file header')
