"""Data-only, checksummed settled generations. Never unpickle a store.

JSON metadata and immutable .npy arrays (allow_pickle=False) are ZIP-compressed.
Full vector materialization is restricted to explicit/checkpoint maintenance,
not a judgment or every ingestion. The complete journal remains authoritative.
"""
from dataclasses import fields
from io import BytesIO
import hashlib
import json
from types import SimpleNamespace
import zipfile
import numpy as np
from immutables import Map


def encode(main):
    from .store import plain, canonical
    graph = main.graph
    arrays = {}
    def array(name, value):
        arrays[name] = np.asarray(value)
        return name
    # Explicit materialization of sparse views for a durable checkpoint.
    numeric = {}
    for name in ('direct','score','strength','unresolved'):
        v = getattr(graph.inputs, name)
        numeric[name] = array('event_'+name, np.fromiter((v[i] for i in range(len(v))), dtype=v.dtype, count=len(v)))
    edges = np.empty(len(graph.inputs.edges), dtype=graph.inputs.edges.dtype)
    for name in edges.dtype.names:
        v = graph.inputs.edges[name]
        edges[name] = np.fromiter((v[i] for i in range(len(v))), dtype=edges.dtype[name], count=len(v))
    numeric['edges'] = array('event_edges', edges)
    regions = []
    for component, (region, positions) in sorted(graph.regions.items()):
        r = {}
        for f in fields(region):
            value = getattr(region, f.name)
            if f.name == 'source':
                continue
            if isinstance(value, np.ndarray):
                r[f.name] = {'array': array(f'region_{component}_{f.name}', value)}
            elif f.name == 'region_terms':
                r[f.name] = [array(f'region_{component}_reverse_{n}', getattr(value,n)) for n in ('offsets','nodes')]
            else:
                r[f.name] = plain(value)
        regions.append([component, r, list(positions.items())])
    metadata = dict(format=1, identity=main.identity, seq=main.sequence,
        pair=main.pair.snapshot_id, memory_snapshot=main.memory.snapshot_id,
        vrs_snapshot=graph.inputs.snapshot_id, numeric=numeric,
        episodes=[plain(e) for e in main.memory.records.values()],
        outcomes=dict(main.memory.outcome_counts), superseded=dict(main.memory.superseded),
        nodes=dict(graph.nodes), components=list(graph.components.items()), regions=regions,
        operations=dict(main.operations), receipt=plain(graph.last_receipt))
    output = BytesIO()
    with zipfile.ZipFile(output,'w',compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('metadata.json', canonical(metadata).encode('utf-8'))
        for name, values in arrays.items():
            body=BytesIO(); np.save(body,values,allow_pickle=False)
            archive.writestr(name+'.npy',body.getvalue())
    return output.getvalue()


def decode(body, *, identity, seq, pair):
    from .store import (HotIndex, Graph, MemoryEpisode, MemoryStep, frozen, retrieval_keys,
        FullCurrentMemoryVrsSnapshot, EventVrsInputs, EndpointDependencyIndex,
        ConnectivityRegions, freeze_view)
    from .engine.mosaic_vrs_region_arrays import RegionTermArrays
    import sys
    try:
        with zipfile.ZipFile(BytesIO(body)) as archive:
            data=json.loads(archive.read('metadata.json').decode('utf-8'))
            if (data['format'],data['identity'],data['seq'],data['pair']) != (1,identity,seq,pair):
                raise ValueError('checkpoint_identity_integrity_failed')
            def array(name):
                return frozen(np.load(BytesIO(archive.read(name+'.npy')),allow_pickle=False))
            records, postings, propositions = {}, {}, {}
            for r in data['episodes']:
                r['steps']=tuple(MemoryStep(**s) for s in r['steps'])
                r['cues']=tuple(sys.intern(c) for c in r['cues'])
                episode=MemoryEpisode(**r)
                records[episode.episode_id]=episode
                for cue in dict.fromkeys((*episode.cues,
                        *retrieval_keys(episode.steps[0].observation['text']))):
                    postings.setdefault(cue,{})[episode.episode_id]=True
                p=episode.steps[0].observation.get('proposition_id')
                if p:
                    propositions.setdefault(p,set()).add(episode.episode_id)
            memory=HotIndex(data['memory_snapshot'],Map(records),Map({k:Map(v) for k,v in postings.items()}),
                Map(data['outcomes']),Map({k:frozenset(v) for k,v in propositions.items()}),Map(data['superseded']))
            numeric={k:array(v) for k,v in data['numeric'].items()}
            inputs=EventVrsInputs(data['vrs_snapshot'],**numeric,dependencies=EndpointDependencyIndex.build(numeric['edges']))
            regions={}
            for component, r, positions in data['regions']:
                for k,v in tuple(r.items()):
                    if isinstance(v,dict) and set(v)=={'array'}:
                        r[k]=array(v['array'])
                r['terms']=tuple(r['terms']); r['sweeps']=tuple(r['sweeps'])
                r['region_terms']=RegionTermArrays(*(array(n) for n in r['region_terms']))
                r['source']=SimpleNamespace(terms=r['terms'],edge_source=r['edge_source'],edge_target=r['edge_target'],edge_sign=r['edge_sign'],vrs_strength=r['strengths'])
                regions[component]=(ConnectivityRegions(**r),Map(positions))
            graph=Graph(inputs,Map(data['nodes']),Map(data['components']),Map(regions),freeze_view(data['receipt']))
            snapshot=FullCurrentMemoryVrsSnapshot(memory,inputs.snapshot_id)
            if snapshot.snapshot_id != pair or len(records)!=sum(memory.outcome_counts.values()):
                raise ValueError('checkpoint_generation_integrity_failed')
            return memory,graph,snapshot,Map({k:tuple(v) for k,v in data['operations'].items()})
    except (KeyError,TypeError,zipfile.BadZipFile,EOFError) as exc:
        raise ValueError('checkpoint_integrity_failed') from exc


def checksum(body):
    return hashlib.sha256(body).hexdigest()
