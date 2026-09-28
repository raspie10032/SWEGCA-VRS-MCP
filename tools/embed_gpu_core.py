from pathlib import Path
root=Path(__file__).resolve().parents[1];target=root/'build/gpu_core_source.hpp'
parts=['#pragma once\nnamespace swegca::vrs::embedded {\n']
for name in ('core_platform','evidence_scalar','association_scalar'):
 text=(root/f'cpp/swegca_architecture/{name}.hpp').read_text();assert ')SWEGCA"' not in text
 parts.append(f'inline constexpr char {name}[]=R"SWEGCA({text})SWEGCA";\n')
parts.append('}\n');target.write_text(''.join(parts))
