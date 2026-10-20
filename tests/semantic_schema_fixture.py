"""Published semantic schemas are checked against real provider projections."""
import functools
import json
from pathlib import Path

from jsonschema import Draft202012Validator

ROOT = Path(__file__).resolve().parents[1]


@functools.lru_cache(maxsize=None)
def semantic_validator(name):
    schema = json.loads((ROOT / 'schemas' / f'builtin-{name}-v1.schema.json').read_text())
    Draft202012Validator.check_schema(schema)
    return Draft202012Validator(schema)


def validate_semantic(value):
    schema_id = value['schema']
    assert schema_id.startswith('jash.') and schema_id.endswith('.v1'), schema_id
    semantic_validator(schema_id[5:-3]).validate(value)
