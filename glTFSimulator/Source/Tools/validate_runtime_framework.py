#!/usr/bin/env python3
from __future__ import annotations
import re, sys, json
from pathlib import Path
source=Path(__file__).resolve().parents[1]
files=[p for p in source.rglob('*') if p.suffix.lower() in {'.h','.hpp','.cpp','.cs'} and 'Plugins' not in p.parts]
reserved=r'INST|MESH|LOD\d+|COL|COLLIDER|LIGHT|CAMERA|SOCKET|PREFAB|VEHICLE|WHEEL|DOOR|SEAT|SPAWN|NAV|NCOL|NOCOL|FORCECOL|HIDDEN|VISIBLE|STATIC|STATIONARY|MOVABLE|SHADOW|NOSHADOW'
forbidden=re.compile(r'\.(?:Contains|StartsWith|EndsWith)\s*\(\s*TEXT\(";?(?:'+reserved+r'|wheel_|_tire|tire_)"\)',re.I)
issues=[]; warnings=[]
required=['USimulatorNodeTokenLibrary','USimulatorInteractionJsonLibrary','USimulatorInteractionAnimInstance','ASimulatorHeldPrefabPreviewActor','USimulatorVehicleSelectionSubsystem','USimulatorBoundsCacheSubsystem','USimulatorSharedResourceSubsystem']
all_text=''
for p in files:
 text=p.read_text(encoding='utf-8',errors='ignore'); all_text+='\n'+text
 if p.name!='SimulatorNodeTokenLibrary.cpp':
  for n,line in enumerate(text.splitlines(),1):
   if forbidden.search(line):issues.append({'kind':'direct_reserved_token_check','file':str(p.relative_to(source)),'line':n,'text':line.strip()})
   if 'AddToRoot(' in line:warnings.append({'kind':'AddToRoot','file':str(p.relative_to(source)),'line':n})
 # Cheap delimiter check catches accidental truncated patches before UBT.
 pairs={'{':'}','(':')','[':']'}; stack=[]; in_string=False; esc=False
 sanitized=re.sub(r'//.*?$|/\*.*?\*/','',text,flags=re.M|re.S)
 for ch in sanitized:
  if in_string:
   if esc:esc=False
   elif ch=='\\':esc=True
   elif ch=='"':in_string=False
   continue
  if ch=='"':in_string=True;continue
  if ch in pairs:stack.append(ch)
  elif ch in pairs.values():
   if not stack or pairs[stack.pop()]!=ch:issues.append({'kind':'delimiter_mismatch','file':str(p.relative_to(source))});break
 else:
  if stack:issues.append({'kind':'unclosed_delimiter','file':str(p.relative_to(source)),'count':len(stack)})
for marker in required:
 if marker not in all_text:issues.append({'kind':'missing_required_marker','marker':marker})
# Generated header must be final include in each reflected header.
for p in files:
 if p.suffix.lower() not in {'.h','.hpp'}:continue
 lines=p.read_text(encoding='utf-8',errors='ignore').splitlines(); gens=[i for i,l in enumerate(lines) if '.generated.h"' in l]
 if gens:
  later=[l for l in lines[gens[-1]+1:] if l.lstrip().startswith('#include')]
  if later:issues.append({'kind':'include_after_generated_header','file':str(p.relative_to(source))})
result={'ok':not issues,'issues':issues,'warnings':warnings,'files_scanned':len(files)}
print(json.dumps(result,ensure_ascii=False,indent=2));sys.exit(0 if not issues else 1)
