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

import asyncio
from pathlib import Path
import pytest
import kor
from kor import Database, db, HexToken, NULL, InvalidConfigurationError, PersistenceError


def test_database_initialization_returns_hex(tmp_path):
    hex_token = Database(tmp_path, "init_test", 3)
    assert isinstance(hex_token, HexToken)
    assert isinstance(hex_token, str)
    assert hex_token.startswith("0x")
    assert len(hex_token) == 34

    # Single .kr file must exist in ROM (disk), and NO .info file
    kr_file = tmp_path / "init_test.kr"
    assert kr_file.exists()
    assert not (tmp_path / "init_test.info").exists()

    # Re-calling initialization on existing db returns existing hex
    hex_token2 = Database(tmp_path, "init_test", 3)
    assert hex_token == hex_token2


def test_subscripting_disallowed(tmp_path):
    hex_token = Database(tmp_path, "no_subscript", 3)
    client = db.connect(tmp_path, "no_subscript", hex_token)

    with pytest.raises(TypeError, match="not subscriptable"):
        _ = client["user:103"]

    client.close()


def test_column_keyword_syntax_and_null_defaults(tmp_path):
    hex_token = Database(tmp_path, "columns_test", 3)
    client = db.connect(tmp_path, "columns_test", hex_token)

    # 1. client.set(key="user:102", c1="Jane Smith", c2="Design", c3=NULL)
    client.set(key="user:102", c1="Jane Smith", c2="Design", c3=NULL)
    res = client.get("user:102")
    assert isinstance(res, list)
    assert res == ["Jane Smith", "Design", None]

    # 2. Unset key returns null for each column
    unset = client.get("nonexistent_user")
    assert isinstance(unset, list)
    assert unset == [None, None, None]

    # 3. Partial column specification (omitted columns default to NULL)
    client.set(key="user:partial", c1="Alice")
    assert client.get("user:partial") == ["Alice", None, None]

    # 4. Updating key with new columns requires overwrite=True
    client.set(key="user:102", c1="Jane Doe", c2="Engineering", c3="Lead", overwrite=True)
    assert client.get("user:102") == ["Jane Doe", "Engineering", "Lead"]

    # 5. Positional arguments also accepted
    client.set("user:103", "Bob", "QA", None)
    assert client.get("user:103") == ["Bob", "QA", None]

    # 6. get_many returns lists with nulls for unset keys
    multi = client.get_many(["user:102", "unset_one"])
    assert multi == [
        ["Jane Doe", "Engineering", "Lead"],
        [None, None, None],
    ]

    client.close()

    # 7. Reconnect from ROM into RAM
    reconnected = Database.connect(tmp_path, "columns_test", hex_token)
    assert reconnected.get("user:102") == ["Jane Doe", "Engineering", "Lead"]
    assert reconnected.get("user:partial") == ["Alice", None, None]
    reconnected.close()


def test_hex_variants_and_authentication(tmp_path):
    hex_token = Database(tmp_path, "auth_test", 2)

    raw_hex = str(hex_token).removeprefix("0x")
    db1 = db.connect(tmp_path, "auth_test", raw_hex)
    assert db1.count() == 0
    db1.close()

    upper_hex = str(hex_token).upper()
    db2 = db.connect(tmp_path, "auth_test", upper_hex)
    db2.close()

    with pytest.raises(InvalidConfigurationError, match="Authentication failed"):
        db.connect(tmp_path, "auth_test", "0x00112233445566778899aabbccddeeff")


def test_token_connect_helper(tmp_path):
    hex_token = Database(tmp_path, "token_helper_test", 2)
    client = hex_token.connect()
    client.set(key="test", c1="col1", c2="col2")
    assert client.get("test") == ["col1", "col2"]
    client.close()


def test_async_connect_with_hex(tmp_path):
    hex_token = Database(tmp_path, "async_test", 2)

    async def run_async():
        async with await kor.async_connect(tmp_path, "async_test", hex=hex_token) as async_db:
            await async_db.set(key="async_key", c1="colA", c2="colB")
            result = await async_db.get("async_key")
            assert isinstance(result, list)
            assert result == ["colA", "colB"]
            assert await async_db.count() == 1

    asyncio.run(run_async())
