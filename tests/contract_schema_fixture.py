"""Independent validation of published JSON and native-value projections."""
import base64
import functools
import json
from pathlib import Path

from jsonschema import Draft202012Validator
from referencing import Registry, Resource

ROOT = Path(__file__).resolve().parents[1]


@functools.lru_cache(maxsize=1)
def schemas():
    documents = {}
    for path in (ROOT / 'schemas').glob('*.schema.json'):
        document = json.loads(path.read_text())
        Draft202012Validator.check_schema(document)
        if '$id' in document:
            assert document['$id'] not in documents, document['$id']
            documents[document['$id']] = document
    registry = Registry().with_resources((key, Resource.from_contents(value))
                                        for key, value in documents.items())
    return documents, registry


def validate_contract(name, value):
    documents, registry = schemas()
    identifier = ('urn:jash:builtin-error:v1' if isinstance(value, dict)
                  and value.get('schema') == 'jash.builtin.error.v1'
                  else 'urn:jash:' + name + ':v1')
    Draft202012Validator(documents[identifier], registry=registry).validate(value)


def projection(value):
    """Project the independently decoded wire value, not production JSON."""
    if value.kind == 'record':
        return {key: projection(item) for key, item in value.value.items()}
    if value.kind == 'list':
        return [projection(item) for item in value.value]
    if value.kind in ('path', 'bytes'):
        if value.kind == 'path':
            try:
                return value.value.decode('utf-8')
            except UnicodeDecodeError:
                pass
        return {'encoding': 'base64', 'data': base64.b64encode(value.value).decode('ascii')}
    return value.value
