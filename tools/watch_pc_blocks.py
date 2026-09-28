#!/usr/bin/env python3
import json,sys,time,subprocess
from pathlib import Path
root=Path(sys.argv[1])
while True:
    print('\033[2J\033[H',end='')
    try:
        s=json.loads((root/'live.json').read_text())
        print('PC 전체 입력 / 블록 내 기권 경험 쌍 혼합\n')
        for name in ('stage','elapsed_seconds','submitted','retained','duplicates','errors','relation_counts','backend_inputs','completed'):
            print(f'{name}: {s.get(name)}')
        print('\nrelation_counts = 승인 / 반려 / 기권. 근거가 없는 쌍은 기권 유지.')
        print('아래는 순간 GPU 사용률이며, 등록 여부를 작업 수행으로 간주하지 않음.\n')
        subprocess.run(['nvidia-smi','--query-gpu=index,name,utilization.gpu,memory.used','--format=csv,noheader'])
        print('\n저장 위치:',root)
    except (OSError,ValueError) as e:print(e)
    time.sleep(2)
