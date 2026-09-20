#!/usr/bin/env python3
"""Validate H-04 contracts and render their compatibility/document views.

Implements: HW-SF-001..005, HW-FR-001..010 (provenance and structural
verification). No physics, requirement text or software gates are changed.
Usage: python3 ci/check_hw_contracts.py . [--export]
The default checks for drift; --export regenerates views only after validation.
"""
from __future__ import annotations

import argparse
import csv
import io
import json
import re
from pathlib import Path

import jsonschema

REGISTRY = 'hw/tests/oracles/registry.json'
BRIDGE = 'hw/model/bridge.json'
TRADES = 'hw/model/trades.json'
CLASSES = {'analytical': '(a)', 'standard': '(b)',
           'golden_measurement': '(c)', 'independent_model': '(d)'}


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f'duplicate JSON key {key!r}')
        result[key] = value
    return result


def read_json(path):
    """Reject duplicate keys rather than silently accepting the last value."""
    return json.loads(Path(path).read_text(encoding='utf-8'),
                      object_pairs_hook=_unique_object)


def validate(document, schema, label):
    """Metaschema and instance validation are mandatory, including formats."""
    jsonschema.Draft202012Validator.check_schema(schema)
    errors = sorted(jsonschema.Draft202012Validator(
        schema, format_checker=jsonschema.FormatChecker()).iter_errors(document),
        key=lambda e: (str(list(e.absolute_path)), e.message))
    if errors:
        raise ValueError('; '.join(f'{label} {list(e.absolute_path)}: {e.message}'
                                   for e in errors))


def load_contract(root, relative, name):
    document = read_json(root / relative)
    schema = read_json(root / 'schemas/hw' / f'hw-{name}-0.1.0.schema.json')
    validate(document, schema, relative)
    return document


def unique_records(records, field, label):
    seen = set()
    for record in records:
        value = record[field]
        if value in seen:
            raise ValueError(f'{label}: duplicate {field} {value!r}')
        seen.add(value)
    return seen


def load_registry(root):
    document = load_contract(root, REGISTRY, 'oracle-registry')
    unique_records(document['oracles'], 'oracle_id', REGISTRY)
    return document


def registry_csv(document):
    """Stable legacy five-column export for check_hw_traceability.py."""
    stream = io.StringIO(newline='')
    writer = csv.writer(stream, lineterminator='\n')
    writer.writerow(['oracle_id', 'oracle', 'class', 'serves', 'validation_gap'])
    for row in sorted(document['oracles'], key=lambda r: r['oracle_id']):
        writer.writerow([row['oracle_id'], row['description'], CLASSES[row['class']],
                         ';'.join(row['serves']), row['validation_gap']])
    return stream.getvalue()


def _cell(value):
    return str(value).replace('|', '&#124;').replace('\n', '<br>')


def markdown_table(headers, rows):
    lines = [headers, ['---'] * len(headers), *rows]
    return '\n'.join('| ' + ' | '.join(_cell(c) for c in row) + ' |'
                     for row in lines)


def registry_table(document):
    return markdown_table(['ID', 'Oracle', 'Class', 'Serves', 'Validation Gap',
                           'Source citation'], [
        [r['oracle_id'], r['description'], r['class'], ', '.join(r['serves']),
         r['validation_gap'], r['source_citation']]
        for r in sorted(document['oracles'], key=lambda r: r['oracle_id'])])


def trades_table(document):
    return markdown_table(
        ['Trade', 'Motivation', 'Owner / target', 'Options / rejection rationale',
         'Selected option', 'Criteria / weight', 'Decision rationale'], [
            [r['trade_id'] + ': ' + r['title'], r['motivating_requirement_or_risk'],
             r['owner'] + ' / ' + r['target_phase'],
             '<br>'.join(o['name'] + ': ' + (o['rationale_for_rejection'] or
                         'not rejected; pending review') for o in r['options_considered']),
             r['selected_option'] or 'Pending (recorded, not decided)',
             '<br>'.join(c['criterion'] + ': ' + (str(c['weight']) if c['weight']
                         is not None else 'pending') for c in r['decision_criteria']),
             r['decision_rationale']]
            for r in sorted(document['trades'], key=lambda r: r['trade_id'])])


def rendered_view(path, marker, table):
    text = path.read_text(encoding='utf-8')
    start, end = f'<!-- BEGIN {marker} -->', f'<!-- END {marker} -->'
    if text.count(start) != 1 or text.count(end) != 1 or text.index(start) >= text.index(end):
        raise ValueError(f'{path.name}: missing/duplicate/reversed {marker} markers')
    before, rest = text.split(start)
    _, after = rest.split(end)
    return before + start + '\n' + table + '\n' + end + after


def check_contracts(root, export=False):
    """Validate all instances first; never generate output from invalid input."""
    registry = load_registry(root)
    load_contract(root, BRIDGE, 'bridge')  # live inventories checked by Capella gate
    trades = load_contract(root, TRADES, 'trade')
    ids = unique_records(trades['trades'], 'trade_id', TRADES)
    if ids != {'T-01', 'T-02', 'T-03', 'T-04'}:
        raise ValueError('trades.json must contain exactly T-01..T-04')
    for trade in trades['trades']:
        options = unique_records(trade['options_considered'], 'name', trade['trade_id'])
        unique_records(trade['decision_criteria'], 'criterion', trade['trade_id'])
        selected = trade['selected_option']
        if selected is not None and selected not in options:
            raise ValueError(f"{trade['trade_id']}: selected_option is not an option")
        if trade['status'] == 'decided':
            if abs(sum(c['weight'] for c in trade['decision_criteria']) - 1.0) > 1e-9:
                raise ValueError(f"{trade['trade_id']}: decided weights must sum to 1")
            for option in trade['options_considered']:
                if option['name'] != selected and not option['rationale_for_rejection']:
                    raise ValueError(f"{trade['trade_id']}: rejected option needs rationale")
    extracts = sorted((root / 'hw/bom/datasheets').glob('*.json'))
    if not extracts:
        raise ValueError('No datasheet extracts found')
    for path in extracts:
        load_contract(root, str(path.relative_to(root)), 'datasheet-extract')
    hwrs = (root / 'docs/hw/HwRS.md').read_text(encoding='utf-8')
    requirements = set(re.findall(r'^\|\s*(HW-(?:SF|FR|NF)-\d{3})\s*\|', hwrs, re.M))
    cited = {req for row in registry['oracles'] for req in row['serves']}
    for trade in trades['trades']:
        refs = set(re.findall(r'HW-(?:SF|FR|NF)-\d{3}', trade['motivating_requirement_or_risk']))
        if not refs:
            raise ValueError(f"{trade['trade_id']}: no motivating HwRS ID")
        cited.update(refs)
    if not requirements or cited - requirements:
        raise ValueError(f'Contracts cite unknown HwRS IDs: {sorted(cited - requirements)}')
    views = {
        root / 'hw/tests/oracles/registry.csv': registry_csv(registry),
        root / 'docs/hw/virtual-bench-plan.md': rendered_view(
            root / 'docs/hw/virtual-bench-plan.md', 'ORACLE REGISTRY', registry_table(registry)),
        root / 'docs/hw/mbse-plan.md': rendered_view(
            root / 'docs/hw/mbse-plan.md', 'TRADES', trades_table(trades)),
    }
    for path, expected in views.items():
        if export:
            path.write_text(expected, encoding='utf-8')
        elif path.read_text(encoding='utf-8') != expected:
            raise ValueError(f'{path.relative_to(root)}: generated view drift; run '
                             'python3 ci/check_hw_contracts.py . --export')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('repo_root', type=Path)
    parser.add_argument('--export', action='store_true')
    args = parser.parse_args(argv)
    try:
        check_contracts(args.repo_root, args.export)
    except (OSError, ValueError, jsonschema.SchemaError) as error:
        print(f'FAIL: hardware contracts: {error}')
        return 1
    print('PASS: hardware contracts and generated views')
    return 0


if __name__ == '__main__':  # pragma: no cover
    raise SystemExit(main())
