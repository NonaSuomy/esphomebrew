#!/usr/bin/env python3
"""Inspect local include closures before staging firmware or distributing source."""
import re
import yaml
from pathlib import Path

def configuration_sources(entry):
    entry=Path(entry).resolve(); root=entry.parent; seen={}
    def included_files(node):
        if node is None:return
        if node.tag=='!include':
            if isinstance(node,yaml.ScalarNode):yield node.value
            elif isinstance(node,yaml.MappingNode):
                files=[v.value for k,v in node.value if k.value=='file' and isinstance(v,yaml.ScalarNode)]
                if len(files)!=1:raise ValueError('Include requires one local file')
                yield files[0]
            else:raise ValueError('Unsupported include form')
        elif node.tag.startswith('!include'):
            raise ValueError('Directory includes need explicit staging support')
        if isinstance(node,yaml.MappingNode):
            for key,value in node.value:yield from included_files(value)
        elif isinstance(node,yaml.SequenceNode):
            for value in node.value:yield from included_files(value)
    def visit(path):
        path=path.resolve()
        if not path.is_relative_to(root):raise ValueError('Config include outside firmware source tree')
        if path in seen:return
        text=path.read_text();seen[path]=text
        for include in included_files(yaml.compose(text)):visit(path.parent/include)
    visit(entry)
    return seen

def public_configuration(entry):
    sources=configuration_sources(entry)
    def station_credentials(node):
        if isinstance(node,yaml.MappingNode):
            for key,value in node.value:
                if key.value=='wifi' and isinstance(value,yaml.MappingNode):
                    if {k.value for k,v in value.value} & {'ssid','password','networks','eap'}:
                        return True
                if station_credentials(value):return True
        elif isinstance(node,yaml.SequenceNode):
            return any(station_credentials(value) for value in node.value)
        return False
    for path,text in sources.items():
        active='\n'.join(line for line in text.splitlines() if not line.lstrip().startswith('#'))
        if '!secret' in active or station_credentials(yaml.compose(text)):
            raise ValueError('Private firmware configuration refused: '+path.name)
    return sources
