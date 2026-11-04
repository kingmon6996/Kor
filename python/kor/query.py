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

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Protocol, TypeVar, runtime_checkable


@runtime_checkable
class KorModel(Protocol):
    def to_dict(self) -> dict[str, Any]: ...

    @classmethod
    def from_dict(cls, value: dict[str, Any]) -> KorModel: ...


ModelT = TypeVar("ModelT", bound=KorModel)


class Filter(Protocol):
    def matches(self, value: Any) -> bool: ...


def _field_value(value: Any, field: str) -> tuple[bool, Any]:
    if not isinstance(field, str) or not field:
        raise TypeError("Filter fields must be non-empty strings.")
    if not isinstance(value, dict):
        return False, None
    return field in value, value.get(field)


@dataclass(frozen=True)
class Eq:
    field: str
    value: Any

    def __post_init__(self) -> None:
        _field_value({}, self.field)

    def matches(self, value: Any) -> bool:
        present, actual = _field_value(value, self.field)
        return present and type(actual) is type(self.value) and actual == self.value


@dataclass(frozen=True)
class Range:
    field: str
    minimum: Any = None
    maximum: Any = None
    include_minimum: bool = True
    include_maximum: bool = True

    def __post_init__(self) -> None:
        _field_value({}, self.field)
        if self.minimum is None and self.maximum is None:
            raise ValueError("A range filter requires a minimum or maximum.")
        if not isinstance(self.include_minimum, bool) or not isinstance(self.include_maximum, bool):
            raise TypeError("Range inclusivity flags must be bool values.")
        bounds = [bound for bound in (self.minimum, self.maximum) if bound is not None]
        if any(isinstance(bound, bool) or not isinstance(bound, (int, float, str)) for bound in bounds):
            raise TypeError("Range bounds must be numbers or strings, excluding bool.")
        if len(bounds) == 2 and type(bounds[0]) is not type(bounds[1]):
            raise TypeError("Range bounds must have the same type.")
        if len(bounds) == 2 and bounds[0] > bounds[1]:
            raise ValueError("Range minimum must not exceed its maximum.")

    def matches(self, value: Any) -> bool:
        present, actual = _field_value(value, self.field)
        if not present or isinstance(actual, bool) or not isinstance(actual, (int, float, str)):
            return False
        bounds = [bound for bound in (self.minimum, self.maximum) if bound is not None]
        if any(type(actual) is not type(bound) for bound in bounds):
            return False
        if self.minimum is not None:
            if actual < self.minimum or (actual == self.minimum and not self.include_minimum):
                return False
        if self.maximum is not None:
            if actual > self.maximum or (actual == self.maximum and not self.include_maximum):
                return False
        return True


@dataclass(frozen=True)
class Exists:
    field: str
    expected: bool = True

    def __post_init__(self) -> None:
        _field_value({}, self.field)
        if not isinstance(self.expected, bool):
            raise TypeError("Exists expected must be a bool.")

    def matches(self, value: Any) -> bool:
        present, _ = _field_value(value, self.field)
        return present is self.expected


@dataclass(frozen=True)
class And:
    filters: tuple[Filter, ...]

    def __init__(self, *filters: Filter) -> None:
        if not filters:
            raise ValueError("And requires at least one filter.")
        object.__setattr__(self, "filters", filters)

    def matches(self, value: Any) -> bool:
        return all(item.matches(value) for item in self.filters)


@dataclass(frozen=True)
class Or:
    filters: tuple[Filter, ...]

    def __init__(self, *filters: Filter) -> None:
        if not filters:
            raise ValueError("Or requires at least one filter.")
        object.__setattr__(self, "filters", filters)

    def matches(self, value: Any) -> bool:
        return any(item.matches(value) for item in self.filters)


@dataclass(frozen=True)
class Not:
    filter: Filter

    def matches(self, value: Any) -> bool:
        return not self.filter.matches(value)
