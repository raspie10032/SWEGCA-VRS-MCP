"""Bounded, lazy transport views over main-owned immutable evidence.

No recursive materialization, inference, source discovery, or cognitive judgment.
The caller owns snapshot immutability and supplies a generation guard. JSON work
is only over the bounded outgoing transport page, never the full evidence tree.
Handles/cursors are request-local; closing the view never deletes main evidence.
"""
from collections.abc import Mapping
from copy import deepcopy
from dataclasses import fields, is_dataclass
from fractions import Fraction
import json
import math


class EvidenceReadRejected(ValueError):
    """Invalid read-only cursor proposal, not a stale view or internal fault."""


def _kind(value):
    if type(value) in (str, bytes, int, float, bool) or value is None:
        if type(value) is float and not math.isfinite(value): raise ValueError('nonfinite evidence scalar')
        return type(value).__name__
    if type(value) is Fraction: return 'fraction'
    if isinstance(value, Mapping):
        if isinstance(value, dict): raise ValueError('immutable mapping view required')
        return 'mapping'
    if type(value) is tuple: return 'sequence'
    if is_dataclass(value) and not isinstance(value,type) and value.__dataclass_params__.frozen:
        return 'record'
    raise ValueError('unsupported or mutable evidence node')


def _size(value, kind):
    if kind in ('str','bytes','mapping','sequence'): return len(value)
    if kind=='record': return len(fields(value))
    if kind=='fraction': return 2
    if kind=='int': return value.bit_length()
    return 1


class HotEvidencePages:
    def __init__(self, root, *, request_id, snapshot_id, guard, maximum_page_bytes,
                 maximum_page_items, maximum_open_cursors):
        if (not isinstance(request_id,str) or not 0<len(request_id)<=128
                or not isinstance(snapshot_id,str) or len(snapshot_id)!=64
                or any(c not in '0123456789abcdef' for c in snapshot_id)
                or not callable(guard)):
            raise ValueError('request, immutable snapshot and main guard required')
        if (type(maximum_page_bytes) is not int or not 1024<=maximum_page_bytes<=1024**2
                or type(maximum_page_items) is not int or not 0<maximum_page_items<=256
                or type(maximum_open_cursors) is not int or maximum_open_cursors<=0):
            raise ValueError('explicit bounded transport capacities required')
        guard()
        _kind(root)
        self.request_id,self.snapshot_id,self.guard=request_id,snapshot_id,guard
        self.maximum_page_bytes,self.maximum_page_items=maximum_page_bytes,maximum_page_items
        self.maximum_open_cursors=maximum_open_cursors
        self._objects,self._identities,self._cursors={}, {}, {}
        self._serial,self._cursor_serial,self._closed=0,0,False
        self._root=self._reference(root)

    def _check(self):
        if self._closed: raise ValueError('evidence view released')
        self.guard()

    def _reference(self,value):
        kind=_kind(value)
        identifier=self._identities.get(id(value))
        if identifier is None:
            identifier=str(self._serial);self._serial+=1
            self._objects[identifier]=value;self._identities[id(value)]=identifier
        return dict(ref=identifier,kind=kind,size=_size(value,kind))

    def _descriptor(self,value):
        kind=_kind(value)
        if value is None or type(value) in (bool,float) or (type(value) is int and value.bit_length()<=128):
            return dict(kind=kind,value=value)
        if type(value) is str and len(value)<=32:
            return dict(kind=kind,value=value)
        return self._reference(value)

    def _reply(self,**data):
        self._check()
        reply=dict(request_id=self.request_id,snapshot_id=self.snapshot_id,
            evidence_is_not_truth=True,grants_authority=False,whole_memory_search=False,**data)
        # The resident wire uses default JSON separators, not compact JSON.
        # ASCII escaping also bounds UTF-8 output, including non-BMP text.
        raw=json.dumps(reply,ensure_ascii=True,allow_nan=False).encode('ascii')
        if len(raw)>self.maximum_page_bytes: raise ValueError('page budget exceeded')
        return reply

    def root(self):
        self._check()
        return self._reply(node=dict(self._root))

    def select(self,reference,key):
        """Random address access; no prefix scan of a large sequence or mapping."""
        self._check()
        value=self._objects[reference];kind=_kind(value)
        if kind=='mapping':
            if type(key) not in (str,int): raise ValueError('literal mapping key required')
            selected=value[key]
        elif kind=='sequence':
            if type(key) is not int or not 0<=key<len(value): raise ValueError('sequence index out of range')
            selected=value[key]
        elif kind=='record':
            if key not in {f.name for f in fields(value)}: raise ValueError('declared evidence field required')
            selected=getattr(value,key)
        elif kind=='fraction':
            if key not in ('numerator','denominator'): raise ValueError('fraction field required')
            selected=getattr(value,key)
        else: raise ValueError('node is not addressable by child key')
        return self._reply(node=self._descriptor(selected))

    def leaf(self,reference,*,offset=0):
        """Exact string/bytes/big-int slices; never stringify an entire large leaf."""
        self._check()
        if type(offset) is not int or offset<0: raise ValueError('nonnegative leaf offset required')
        value=self._objects[reference];kind=_kind(value)
        if kind in ('str','bytes'):
            count=len(value)
            if offset>count: raise ValueError('leaf offset out of range')
            # Worst-case JSON escaping is twelve ASCII bytes per Unicode character.
            length=min(count-offset,max(1,(self.maximum_page_bytes-768)//12))
            chunk=value[offset:offset+length]
            content=chunk if kind=='str' else chunk.hex()
            return self._reply(kind=kind,ref=reference,offset=offset,
                next_offset=offset+length if offset+length<count else None,total=count,
                encoding='unicode_codepoints' if kind=='str' else 'hex_bytes',content=content)
        if kind=='int':
            count=max(1,(value.bit_length()+63)//64)
            if offset>=count: raise ValueError('integer limb offset out of range')
            return self._reply(kind=kind,ref=reference,offset=offset,total=count,
                next_offset=offset+1 if offset+1<count else None,encoding='little_endian_uint64_limbs',
                negative=value<0,content=(abs(value)>>(offset*64))&((1<<64)-1))
        if offset: raise ValueError('scalar offset must be zero')
        return self._reply(node=self._descriptor(value))

    def _entries(self,value):
        kind=_kind(value)
        if kind=='mapping': return iter(value.items())
        if kind=='sequence': return iter(enumerate(value))
        if kind=='record': return ((f.name,getattr(value,f.name)) for f in fields(value))
        if kind=='fraction': return iter((('numerator',value.numerator),('denominator',value.denominator)))
        raise ValueError('container required for page cursor')

    def _project(self, value, paths):
        """Read only requested fields; previews retain exact original leaf refs."""
        rows=[]
        preview_chars=min(96,max(1,(self.maximum_page_bytes-768)//(24*len(paths))))
        for path in paths:
            selected=value
            try:
                for key in path:
                    kind=_kind(selected)
                    if kind=='mapping': selected=selected[key]
                    elif kind=='sequence':
                        if type(key) is not int or not 0<=key<len(selected): raise KeyError(key)
                        selected=selected[key]
                    elif kind=='record':
                        if key not in {f.name for f in fields(selected)}: raise KeyError(key)
                        selected=getattr(selected,key)
                    elif kind=='fraction':
                        if key not in ('numerator','denominator'): raise KeyError(key)
                        selected=getattr(selected,key)
                    else: raise KeyError(key)
            except KeyError:
                rows.append(dict(path=list(path),unavailable=True,error='field_not_present'))
                continue
            node=self._descriptor(selected)
            if type(selected) is str and 'ref' in node:
                end=min(len(selected),preview_chars)
                node=dict(node,preview=selected[:end],preview_start=0,preview_end=end,
                          preview_truncated=end<len(selected),encoding='unicode_codepoints')
            rows.append(dict(path=list(path),node=node))
        return rows

    def page(self,reference,*,cursor=None,field_paths=None):
        """Forward cursor with repeat-last idempotence and O(page-items) traversal.

        Old expired cursors are explicit errors. Reopen or select a known address;
        cursor capacity is temporary backpressure, never inaccessible experience.
        """
        self._check()
        if field_paths is not None:
            if (type(field_paths) not in (list,tuple) or not 1<=len(field_paths)<=8
                    or any(type(path) not in (list,tuple) or not 1<=len(path)<=16
                        or any(type(key) not in (str,int) for key in path) for path in field_paths)):
                raise ValueError('bounded projection field paths required')
            field_paths=tuple(tuple(path) for path in field_paths)
        if cursor is None:
            if len(self._cursors)>=self.maximum_open_cursors:
                raise EvidenceReadRejected('release an evidence cursor before opening another')
            value=self._objects[reference]
            iterator=self._entries(value)
            identifier=str(self._cursor_serial);self._cursor_serial+=1
            state=dict(ref=reference,iterator=iterator,pending=None,step=0,index=0,last=None,
                last_input=None,finished=False,field_paths=field_paths)
            self._cursors[identifier]=state
        else:
            if not isinstance(cursor,str) or len(cursor)>80 or cursor.count(':')!=1:
                raise EvidenceReadRejected('bounded evidence cursor required')
            identifier,step=cursor.split(':')
            state=self._cursors[identifier]
            if state['ref']!=reference: raise EvidenceReadRejected('cursor belongs to another evidence node')
            if state['field_paths']!=field_paths: raise EvidenceReadRejected('cursor projection changed')
            if cursor==state['last_input']:
                self._check();return deepcopy(state['last'])
            if step!=str(state['step']) or state['finished']:
                raise EvidenceReadRejected('expired evidence cursor; reopen or select exact address')
        rows=[];start=state['index'];exhausted=False
        try:
            while len(rows)<self.maximum_page_items:
                if state['pending'] is None:
                    try: state['pending']=next(state['iterator'])
                    except StopIteration: exhausted=True;break
                key,value=state['pending']
                row=dict(key=self._descriptor(key),value=self._descriptor(value))
                if field_paths is not None: row['fields']=self._project(value,field_paths)
                trial=dict(entries=rows+[row],ref=reference,offset=start,
                    next_cursor=identifier+':'+str(state['step']+1),cursor_id=identifier,
                    container_complete=False)
                try: self._reply(**trial)
                except ValueError as error:
                    if str(error)!='page budget exceeded': raise
                    if rows: break
                    if field_paths is not None: raise ValueError('projection exceeds page budget; request fewer fields')
                    # Even two short non-BMP strings can exceed a small page's
                    # escaped-JSON budget. Keep exact leaves addressable by ref.
                    row=dict(key=self._reference(key),value=self._reference(value))
                    trial['entries']=[row]
                    self._reply(**trial)
                rows.append(row);state['pending']=None;state['index']+=1
            result=self._reply(entries=rows,ref=reference,offset=start,
                next_cursor=None if exhausted else identifier+':'+str(state['step']+1),
                cursor_id=identifier,container_complete=exhausted)
        except BaseException:
            self._cursors.pop(identifier,None)
            raise
        state['step']+=1;state['finished']=exhausted
        state['last_input']=cursor;state['last']=deepcopy(result)
        return result

    def release_cursor(self,identifier):
        # Allow cleanup even after generation replacement. No source mutation.
        self._cursors.pop(identifier)

    def close(self):
        self._objects.clear();self._identities.clear();self._cursors.clear()
        self._root=None;self.guard=None;self._closed=True
