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

import kor
from kor import _kor


def main() -> None:
    print(f"kor.__version__ = {kor.__version__}")
    print(f"_kor.native_version() = {_kor.native_version()}")

    engine = _kor.Engine()
    print(f"engine.started = {engine.is_started()}")
    engine.start()
    print(f"engine.started after start = {engine.is_started()}")
    engine.close()
    print(f"engine.started after close = {engine.is_started()}")


if __name__ == "__main__":
    main()
