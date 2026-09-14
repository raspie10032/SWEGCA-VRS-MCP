"""Synthetic native wire peer, not a native cognition or VRS implementation."""
from contextlib import contextmanager
import json
import socketserver
import threading
from uuid import uuid4

OUTCOMES = ('success', 'failure', 'negative', 'uncertain', 'conflict', 'pending')


class NativeFixture:
    def __init__(self):
        self.snapshot = 'a'*64
        self.sessions = {}
        self.commands = []
        self.replay = []
        self.candidates = []
        self.verdicts = []
        for i, outcome in enumerate(OUTCOMES):
            identity = 'synthetic:'+str(i)
            self.candidates.append(dict(episode_id=identity,revision='revision:1',matched_cues=['synthetic'],
                verification_state='unverified',historical_outcomes=[outcome]))
            self.replay.append(dict(episode_id=identity,source_addresses=['fixture:'+str(i)],
                verification_state='unverified',historical_truth_authorized=False,
                steps=[dict(phase='observe',observation={'text':'합성 관측 '+outcome},relations=[],
                    judgment='recorded, not current truth',outcome=outcome,evidence_refs=['fixture:'+str(i)])]))
            self.verdicts.append(dict(episode_id=identity,proposition='synthetic-proposition',verdict='available',
                rationale='record available, not new factual support',current_evidence_refs=['snapshot:'+self.snapshot],
                contradiction_refs=[]))
        selection=dict(selected_cues=['synthetic'],rejected_cues=['missing'],
                       selection_method='synthetic_main_fixture',cue_reasons={'missing':'hot_key_miss'})
        self.root=dict(memory_selection=selection,vrs_selection=selection,
            current_strengths={r['episode_id']:0.5 for r in self.replay},
            current_promotions={r['episode_id']:False for r in self.replay},
            current_propositions={r['episode_id']:'synthetic-proposition' for r in self.replay},
            receipt={'activation':{'stage_order':['deja_vu','recall','replay','re_evidence'],
                'recall':{'candidates':self.candidates},'replay':{'episodes':self.replay},
                're_evidence':dict(query='synthetic',judgments=self.verdicts,selected_support=[],
                    selected_refutation=[],conflicting_propositions=[],unresolved_conflict=False,
                    insufficient_evidence=True,should_abstain=True)}})

    def request(self, command, **args):
        self.commands.append(command)
        if command == 'status':
            return dict(status='synthetic_ready',pair_snapshot_id=self.snapshot,
                        hot_episode_count=6,scheduled_dialogue_available=True,lookup_requires_io=False)
        identifier=args['request_id']
        if command == 'cognitive_dialogue_start':
            assert args['expected_pair_snapshot_id']==self.snapshot and identifier not in self.sessions
            self.sessions[identifier]=dict(view=uuid4().hex,snapshot=self.snapshot,turns=0,objects={},cursors={})
            return dict(status='queued',view_id=self.sessions[identifier]['view'])
        session=self.sessions[identifier]
        assert args['view_id']==session['view']
        if command == 'cognitive_dialogue_release':
            del self.sessions[identifier]
            return dict(status='released')
        assert session['snapshot']==self.snapshot
        if command == 'cognitive_dialogue_continue':
            session['turns']+=1
            return dict(status='pending')
        def node(value):
            kind=('mapping' if type(value) is dict else 'sequence' if type(value) is list else
                  'NoneType' if value is None else type(value).__name__)
            if value is None or type(value) in (bool,float,int) or (type(value) is str and len(value)<=32):
                return dict(kind=kind,value=value)
            key=str(id(value));session['objects'][key]=value
            return dict(ref=key,kind=kind,size=len(value))
        envelope=dict(request_id=identifier,view_id=session['view'],snapshot_id=session['snapshot'],
                      grants_authority=False,evidence_is_not_truth=True,whole_memory_search=False)
        if command == 'cognitive_dialogue_evidence_open':
            if session['turns']<2: return dict(status='pending')
            return dict(envelope,status='evidence_ready',node=node(self.root))
        assert command=='cognitive_dialogue_evidence'
        op=args['operation']
        if op=='root': return dict(envelope,node=node(self.root))
        if op=='release_cursor':
            session['cursors'].pop(args['cursor_id'])
            return dict(envelope,node=node(self.root))
        value=session['objects'][args['reference']]
        if op=='select': return dict(envelope,node=node(value[args['key']]))
        if op=='leaf':
            offset=args.get('offset',0);end=min(len(value),offset+128)
            return dict(envelope,content=value[offset:end],next_offset=end if end<len(value) else None,
                        total=len(value),offset=offset,kind='str')
        assert op=='page'
        cursor=args.get('cursor')
        if cursor:
            key,position=cursor.split(':');position=int(position)
            assert session['cursors'][key]==args['reference']
        else:
            assert len(session['cursors'])<2
            key=uuid4().hex;position=0;session['cursors'][key]=args['reference']
        entries=list(value.items()) if type(value) is dict else list(enumerate(value))
        end=min(len(entries),position+2)
        return dict(envelope,entries=[dict(key=node(k),value=node(v)) for k,v in entries[position:end]],
            next_cursor=f'{key}:{end}' if end<len(entries) else None,cursor_id=key,
            container_complete=end==len(entries))


@contextmanager
def native_peer(path):
    fixture=NativeFixture()
    class Handler(socketserver.StreamRequestHandler):
        def handle(self):
            try:
                data=json.loads(self.rfile.readline(1048577));command=data.pop('command')
                reply=fixture.request(command,**data)
            except (AssertionError,KeyError,ValueError): reply={'status':'rejected'}
            self.wfile.write(json.dumps(reply,ensure_ascii=False).encode()+b'\n')
    with socketserver.UnixStreamServer(str(path),Handler) as server:
        thread=threading.Thread(target=server.serve_forever,kwargs={'poll_interval':.01});thread.start()
        try: yield fixture
        finally: server.shutdown();thread.join(timeout=5)
