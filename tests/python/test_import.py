# Copyright 2026 Pravanjan Roy (kingmon6996)
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import json
import importlib
from pathlib import Path

import kor
import pytest
from kor import Database, InvalidValueError, PersistenceError, _kor


def test_package_and_extension_import():
    assert kor.__version__ == "0.1.0"
    assert hasattr(kor, "Engine")
    assert hasattr(kor, "Database")
    assert hasattr(kor, "native_version")
    assert hasattr(kor, "InvalidValueError")
    assert _kor.native_version() == kor.__version__
    assert importlib.import_module("kor") is kor
    db = Database(path="database", name="store", max_memory=64, snapshot_interval=60)
    assert db.path == Path("database").resolve()


def test_python_integer_conversion_preserves_unsigned_64_bit_values(tmp_path):
    database = Database(path=tmp_path, name="unsigned-values", max_memory=16, snapshot_interval=60)
    maximum_unsigned = 2**64 - 1
    database.set("maximum", maximum_unsigned)
    assert database.get("maximum") == maximum_unsigned
    with pytest.raises(InvalidValueError):
        database.set("overflow", 2**64)
    assert database.get("overflow") is None
    database.close()


def test_jsonl_import_accepts_legacy_schema_version_one(tmp_path):
    source = tmp_path / "legacy.jsonl"
    source.write_text(
        "\n".join(
            [
                json.dumps({"kor_format": "jsonl", "schema_version": 1, "record_count": 1}),
                json.dumps({"key": "legacy", "value": {"integer": 2**53 + 1}}),
                "",
            ]
        ),
        encoding="utf-8",
    )
    database = Database(path=tmp_path, name="legacy-import", max_memory=16, snapshot_interval=60)
    assert database.import_data(source)["record_count"] == 1
    assert database.get("legacy") == {"integer": 2**53 + 1}
    database.close()


def test_jsonl_import_rejects_malformed_tags_and_record_count_without_mutation(tmp_path):
    database = Database(path=tmp_path, name="invalid-jsonl", max_memory=16, snapshot_interval=60)
    malformed_tag = tmp_path / "malformed-tag.jsonl"
    malformed_tag.write_text(
        "\n".join(
            [
                json.dumps({"kor_format": "jsonl", "schema_version": 2, "record_count": 2}),
                json.dumps({"key": "valid", "value": 1}),
                json.dumps({"key": "invalid", "value": {"$kor": "bytes", "data": "%%%"} }),
                "",
            ]
        ),
        encoding="utf-8",
    )
    with pytest.raises(PersistenceError, match="base64"):
        database.import_data(malformed_tag)
    assert database.count() == 0

    wrong_count = tmp_path / "wrong-count.jsonl"
    wrong_count.write_text(
        "\n".join(
            [
                json.dumps({"kor_format": "jsonl", "schema_version": 1, "record_count": 2}),
                json.dumps({"key": "only-one", "value": True}),
                "",
            ]
        ),
        encoding="utf-8",
    )
    with pytest.raises(PersistenceError, match="record count mismatch"):
        database.import_data(wrong_count)
    assert database.count() == 0
    database.close()
