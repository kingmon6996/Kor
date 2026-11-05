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

import pytest

import kor
from kor import _kor


def test_engine_lifecycle():
    engine = _kor.Engine()
    assert engine.is_started() is False

    engine.start()
    assert engine.is_started() is True

    engine.close()
    assert engine.is_started() is False

    engine.start()
    assert engine.is_started() is True

    engine.close()
    assert engine.is_started() is False


def test_native_exceptions_are_mapped():
    with pytest.raises(_kor.InitializationError):
        raise _kor.InitializationError("bootstrap failed")

    with pytest.raises(_kor.Error):
        raise _kor.InitializationError("bootstrap failed")

    assert issubclass(_kor.InitializationError, _kor.Error)
    assert kor.Engine is _kor.Engine
