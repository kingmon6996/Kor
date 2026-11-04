#include <pybind11/pybind11.h>
#include <pybind11/stl/filesystem.h>
#include <pybind11/stl.h>
#include <unordered_set>

#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "kor/config.hpp"
#include "kor/database_file.hpp"
#include "kor/engine.hpp"
#include "kor/exceptions.hpp"
#include "kor/memory_tracker.hpp"
#include "kor/storage.hpp"
#include "kor/transaction.hpp"
#include "kor/value.hpp"
#include "kor/version.hpp"
#include "kor/wal.hpp"

namespace py = pybind11;

namespace {

kor::Value py_to_value(py::handle obj, std::size_t depth = 0) {
    if (depth > kor::kMaxValueNestingDepth) {
        throw kor::InvalidValueError("Value nesting exceeds the supported maximum depth.");
    }

    if (obj.is_none()) {
        return kor::Value(nullptr);
    }

    if (py::isinstance<py::bool_>(obj)) {
        return kor::Value(obj.cast<bool>());
    }

    if (py::isinstance<py::int_>(obj)) {
        int overflow = 0;
        const auto signed_value = PyLong_AsLongLongAndOverflow(obj.ptr(), &overflow);
        if (overflow == 0) {
            return kor::Value(static_cast<std::int64_t>(signed_value));
        }
        if (overflow > 0) {
            const auto unsigned_value = PyLong_AsUnsignedLongLong(obj.ptr());
            if (unsigned_value == static_cast<unsigned long long>(-1) && PyErr_Occurred()) {
                PyErr_Clear();
                throw kor::InvalidValueError("Integer values must fit in a signed or unsigned 64-bit integer.");
            }
            return kor::Value(static_cast<std::uint64_t>(unsigned_value));
        }
        throw kor::InvalidValueError("Integer values must fit in a signed or unsigned 64-bit integer.");
    }

    if (py::isinstance<py::float_>(obj)) {
        const double number = obj.cast<double>();
        if (!std::isfinite(number)) {
            throw kor::InvalidValueError("Floating-point values must be finite.");
        }
        return kor::Value(number);
    }

    if (py::isinstance<py::str>(obj)) {
        return kor::Value(obj.cast<std::string>());
    }

    if (py::isinstance<py::bytes>(obj)) {
        const auto data = obj.cast<std::string>();
        return kor::Value(kor::Bytes(data.begin(), data.end()));
    }

    if (py::isinstance<py::bytearray>(obj)) {
        const auto data = obj.cast<std::string>();
        return kor::Value(kor::Bytes(data.begin(), data.end()));
    }

    if (py::isinstance<py::list>(obj) || py::isinstance<py::tuple>(obj)) {
        py::iterable iterable = py::reinterpret_borrow<py::iterable>(obj);
        kor::ValueList items;
        for (const py::handle entry : iterable) {
            items.push_back(py_to_value(entry, depth + 1U));
        }
        return kor::Value(std::move(items));
    }

    if (py::isinstance<py::dict>(obj)) {
        py::dict mapping = py::reinterpret_borrow<py::dict>(obj);
        kor::ValueMap values;
        for (const auto item : mapping) {
            const auto key = py::cast<std::string>(item.first);
            values[key] = py_to_value(item.second, depth + 1U);
        }
        return kor::Value(std::move(values));
    }

    throw kor::InvalidValueError("Unsupported Python value for native storage.");
}

py::object value_to_py(const kor::Value& value) {
    if (value.is_null()) {
        return py::none();
    }
    if (value.is_bool()) {
        return py::bool_(std::get<bool>(value.data));
    }
    if (value.is_int64()) {
        return py::int_(std::get<std::int64_t>(value.data));
    }
    if (value.is_uint64()) {
        return py::int_(std::get<std::uint64_t>(value.data));
    }
    if (value.is_double()) {
        return py::float_(std::get<double>(value.data));
    }
    if (value.is_string()) {
        return py::str(std::get<std::string>(value.data));
    }
    if (value.is_bytes()) {
        const auto& bytes = std::get<kor::Bytes>(value.data);
        return py::bytes(reinterpret_cast<const char*>(bytes.data()), static_cast<py::ssize_t>(bytes.size()));
    }
    if (value.is_list()) {
        py::list list;
        for (const auto& item : std::get<kor::ValueList>(value.data)) {
            list.append(value_to_py(item));
        }
        return list;
    }
    if (value.is_map()) {
        py::dict dictionary;
        for (const auto& entry : std::get<kor::ValueMap>(value.data)) {
            dictionary[entry.first.c_str()] = value_to_py(entry.second);
        }
        return dictionary;
    }

    throw kor::InvalidValueError("Value cannot be converted back to Python.");
}

std::uint64_t parse_storage_limit(py::args args, py::kwargs kwargs) {
    std::uint64_t limit = 64ULL * 1024ULL * 1024ULL;

    if (kwargs.contains("max_memory_bytes")) {
        limit = py::cast<std::uint64_t>(kwargs["max_memory_bytes"]);
        return limit;
    }

    if (kwargs.contains("max_memory_mib")) {
        limit = py::cast<std::uint64_t>(kwargs["max_memory_mib"]) * 1024ULL * 1024ULL;
        return limit;
    }

    if (kwargs.contains("max_memory")) {
        limit = py::cast<std::uint64_t>(kwargs["max_memory"]) * 1024ULL * 1024ULL;
        return limit;
    }

    if (args.size() > 0) {
        limit = py::cast<std::uint64_t>(args[0]);
        if (limit < 1024ULL) {
            limit *= 1024ULL * 1024ULL;
        }
    }

    return limit;
}

void commit_without_gil(kor::Transaction& transaction) {
    py::gil_scoped_release release;
    transaction.commit();
}

}  // namespace

PYBIND11_MODULE(_kor, m) {
    m.doc() = "Native Kor engine bindings.";

    m.def("native_version", []() { return kor::version(); }, "Return the native Kor version string.");
    m.def("make_config",
          [](const std::filesystem::path& path,
             const std::string& name,
             std::uint64_t max_memory_mib,
             std::uint32_t snapshot_interval_seconds) {
              return kor::make_config(path, name, max_memory_mib, snapshot_interval_seconds);
          },
          py::arg("path"),
          py::arg("name"),
          py::arg("max_memory_mib"),
          py::arg("snapshot_interval_seconds"),
          "Validate and normalize a database configuration.");
    m.def("initialize_database_file",
          [](const std::filesystem::path& path,
             const std::string& name,
             std::uint64_t max_memory_mib,
             std::uint32_t snapshot_interval_seconds) {
              py::gil_scoped_release release;
              return kor::initialize_database_file(path, name, max_memory_mib, snapshot_interval_seconds);
          },
          py::arg("path"),
          py::arg("name"),
          py::arg("max_memory_mib"),
          py::arg("snapshot_interval_seconds"),
          "Create a valid single-file Kor database if it does not already exist.");
    m.def("database_file_is_valid",
          [](const std::filesystem::path& path) {
              py::gil_scoped_release release;
              return kor::database_file_is_valid(path);
          },
          py::arg("path"),
          "Return whether a path contains a valid single-file Kor database header.");
    m.def("read_database_config",
          [](const std::filesystem::path& path) {
              kor::DatabaseConfig config;
              {
                  py::gil_scoped_release release;
                  config = kor::read_database_config(path);
              }
              py::dict result;
              result["path"] = path;
              result["name"] = config.name;
              result["max_memory_mib"] = config.max_memory_mib;
              result["snapshot_interval_seconds"] = config.snapshot_interval_seconds;
              result["persistence"] = config.persistence;
              result["durability"] = config.durability;
              return result;
          },
          py::arg("path"),
          "Read the metadata stored in a Kor database file.");

    py::exception<kor::Error> error(m, "Error");
    error.doc() = "Base exception for native Kor engine errors.";

    py::register_exception<kor::InitializationError>(m, "InitializationError", error);
    py::register_exception<kor::InvalidConfigurationError>(m, "InvalidConfigurationError", error);
    py::register_exception<kor::InvalidKeyError>(m, "InvalidKeyError", error);
    py::register_exception<kor::InvalidValueError>(m, "InvalidValueError", error);
    py::register_exception<kor::SerializationError>(m, "SerializationError", error);
    py::register_exception<kor::MemoryLimitExceededError>(m, "MemoryLimitExceededError", error);
    py::register_exception<kor::StorageClosedError>(m, "StorageClosedError", error);
    py::register_exception<kor::ReadOnlyError>(m, "ReadOnlyError", error);
    py::register_exception<kor::DatabaseFormatError>(m, "DatabaseFormatError", error);
    py::register_exception<kor::UnsupportedFormatVersionError>(m, "UnsupportedFormatVersionError", error);
    py::register_exception<kor::PersistenceError>(m, "PersistenceError", error);
    py::register_exception<kor::RecoveryError>(m, "RecoveryError", error);
    py::register_exception<kor::TransactionError>(m, "TransactionError", error);
    py::register_exception<kor::TransactionStateError>(m, "TransactionStateError", error);
    py::register_exception<kor::TransactionTooLargeError>(m, "TransactionTooLargeError", error);
    py::register_exception<kor::TransactionCommitError>(m, "TransactionCommitError", error);
    py::register_exception<kor::IndexError>(m, "IndexError", error);
    py::register_exception<kor::InvalidIndexDefinitionError>(m, "InvalidIndexDefinitionError", error);
    py::register_exception<kor::IndexExistsError>(m, "IndexExistsError", error);
    py::register_exception<kor::IndexNotFoundError>(m, "IndexNotFoundError", error);
    py::register_exception<kor::UniqueConstraintError>(m, "UniqueConstraintError", error);
    py::register_exception<kor::UnsupportedIndexValueError>(m, "UnsupportedIndexValueError", error);

    py::class_<kor::MemoryStats>(m, "MemoryStats")
        .def_readonly("limit_bytes", &kor::MemoryStats::limit_bytes)
        .def_readonly("key_bytes", &kor::MemoryStats::key_bytes)
        .def_readonly("value_bytes", &kor::MemoryStats::value_bytes)
        .def_readonly("record_overhead_bytes", &kor::MemoryStats::record_overhead_bytes)
        .def_readonly("index_bytes", &kor::MemoryStats::index_bytes)
        .def_readonly("total_bytes", &kor::MemoryStats::total_bytes)
        .def_readonly("records", &kor::MemoryStats::records);

    py::class_<kor::StorageStats>(m, "StorageStats")
        .def_readonly("record_count", &kor::StorageStats::record_count)
        .def_readonly("configured_limit_bytes", &kor::StorageStats::configured_limit_bytes)
        .def_readonly("key_bytes", &kor::StorageStats::key_bytes)
        .def_readonly("value_bytes", &kor::StorageStats::value_bytes)
        .def_readonly("record_overhead_bytes", &kor::StorageStats::record_overhead_bytes)
        .def_readonly("index_bytes", &kor::StorageStats::index_bytes)
        .def_readonly("total_bytes", &kor::StorageStats::total_bytes)
        .def_readonly("file_size_bytes", &kor::StorageStats::file_size_bytes)
        .def_readonly("logical_live_data_bytes_estimate", &kor::StorageStats::logical_live_data_bytes_estimate)
        .def_readonly("wal_bytes_required", &kor::StorageStats::wal_bytes_required)
        .def_readonly("snapshot_bytes_retained", &kor::StorageStats::snapshot_bytes_retained)
        .def_readonly("allocated_region_bytes", &kor::StorageStats::allocated_region_bytes)
        .def_readonly("reusable_free_space_bytes", &kor::StorageStats::reusable_free_space_bytes)
        .def_readonly("obsolete_bytes_estimate", &kor::StorageStats::obsolete_bytes_estimate)
        .def_readonly("internal_fragmentation_estimate", &kor::StorageStats::internal_fragmentation_estimate)
        .def_readonly("last_checkpoint_lsn", &kor::StorageStats::last_checkpoint_lsn)
        .def_readonly("compaction_generation", &kor::StorageStats::compaction_generation)
        .def_readonly("physical_shrinking_supported", &kor::StorageStats::physical_shrinking_supported)
        .def_readonly("closed", &kor::StorageStats::closed);

    py::class_<kor::CompactionResult>(m, "CompactionResult")
        .def_readonly("file_size_before_bytes", &kor::CompactionResult::file_size_before_bytes)
        .def_readonly("file_size_after_bytes", &kor::CompactionResult::file_size_after_bytes)
        .def_readonly("logical_wal_bytes_reclaimed", &kor::CompactionResult::logical_wal_bytes_reclaimed)
        .def_readonly("file_length_reduction_bytes", &kor::CompactionResult::file_length_reduction_bytes)
        .def_readonly("checkpoint_staging_memory_estimate_bytes",
                      &kor::CompactionResult::checkpoint_staging_memory_estimate_bytes)
        .def_readonly("checkpoint_lsn", &kor::CompactionResult::checkpoint_lsn)
        .def_readonly("compaction_generation", &kor::CompactionResult::compaction_generation)
        .def_readonly("checkpoint_performed", &kor::CompactionResult::checkpoint_performed)
        .def_readonly("file_physically_shrunk", &kor::CompactionResult::file_physically_shrunk)
        .def_readonly("limitation", &kor::CompactionResult::limitation);

    py::class_<kor::IndexInfo>(m, "IndexInfo")
        .def_readonly("name", &kor::IndexInfo::name)
        .def_readonly("field", &kor::IndexInfo::field)
        .def_readonly("fields", &kor::IndexInfo::fields)
        .def_readonly("index_type", &kor::IndexInfo::index_type)
        .def_readonly("unique", &kor::IndexInfo::unique)
        .def_readonly("generation", &kor::IndexInfo::generation)
        .def_readonly("entry_count", &kor::IndexInfo::entry_count);

    py::class_<kor::Value>(m, "Value")
        .def(py::init<>())
        .def(py::init([](py::handle obj) { return kor::Value(py_to_value(obj)); }))
        .def("is_null", &kor::Value::is_null)
        .def("is_bool", &kor::Value::is_bool)
        .def("is_int64", &kor::Value::is_int64)
        .def("is_uint64", &kor::Value::is_uint64)
        .def("is_double", &kor::Value::is_double)
        .def("is_string", &kor::Value::is_string)
        .def("is_bytes", &kor::Value::is_bytes)
        .def("is_list", &kor::Value::is_list)
        .def("is_map", &kor::Value::is_map)
        .def("__repr__", [](const kor::Value& value) { return value.is_string() ? py::str(std::get<std::string>(value.data)) : py::str("<kor.Value>"); })
        .def("__eq__", [](const kor::Value& lhs, const kor::Value& rhs) { return lhs == rhs; });

    py::class_<kor::Storage>(m, "Storage")
        .def(py::init([](py::args args, py::kwargs kwargs) {
                 auto limit = parse_storage_limit(args, kwargs);
                 py::gil_scoped_release release;
                 return new kor::Storage(limit);
             }))
        .def("set",
             [](kor::Storage& storage, const std::string& key, py::handle value) {
                 const auto native_value = py_to_value(value);
                 py::gil_scoped_release release;
                 return storage.set(key, native_value);
             },
             py::arg("key"),
             py::arg("value"))
        .def("get",
             [](kor::Storage& storage, const std::string& key, py::object default_value) -> py::object {
                 std::optional<kor::Value> found;
                 {
                     py::gil_scoped_release release;
                     found = storage.get(key);
                 }
                 if (!found.has_value()) {
                     return default_value;
                 }
                 return value_to_py(*found);
             },
             py::arg("key"),
             py::arg("default") = py::none())
        .def("get_many",
             [](const kor::Storage& storage, const std::vector<std::string>& keys) {
                 std::vector<std::optional<kor::Value>> values;
                 {
                     py::gil_scoped_release release;
                     values = storage.get_many(keys);
                 }
                 py::list result;
                 for (const auto& value : values) {
                     result.append(value.has_value() ? value_to_py(*value) : py::none());
                 }
                 return result;
             },
             py::arg("keys"))
        .def("contains_many",
             [](const kor::Storage& storage, const std::vector<std::string>& keys) {
                 py::gil_scoped_release release;
                 return storage.contains_many(keys);
             },
             py::arg("keys"))
        .def("exists",
             [](const kor::Storage& storage, const std::string& key) {
                 py::gil_scoped_release release;
                 return storage.exists(key);
             },
             py::arg("key"))
        .def("erase",
             [](kor::Storage& storage, const std::string& key) {
                 py::gil_scoped_release release;
                 return storage.erase(key);
             },
             py::arg("key"))
        .def("count", [](const kor::Storage& storage) {
            py::gil_scoped_release release;
            return storage.count();
        })
        .def("list_records",
             [](const kor::Storage& storage) {
                 std::vector<std::pair<std::string, kor::Value>> rows;
                 {
                     py::gil_scoped_release release;
                     rows = storage.list_records();
                 }
                 py::list output;
                 for (const auto& [key, value] : rows) {
                     output.append(py::make_tuple(key, value_to_py(value)));
                 }
                 return output;
             })
        .def("is_empty", [](const kor::Storage& storage) {
            py::gil_scoped_release release;
            return storage.is_empty();
        })
        .def("clear", [](kor::Storage& storage) {
            py::gil_scoped_release release;
            storage.clear();
        })
        .def("close", [](kor::Storage& storage) {
            py::gil_scoped_release release;
            storage.close();
        })
        .def("is_closed", &kor::Storage::is_closed)
        .def("is_read_only", &kor::Storage::is_read_only)
        .def("stop_auto_snapshots", [](kor::Storage& storage) {
            storage.stop_auto_snapshot_worker();
        })
        .def("set_if_absent",
             [](kor::Storage& storage, const std::string& key, py::handle value) {
                 const auto native_value = py_to_value(value);
                 py::gil_scoped_release release;
                 return storage.set_if_absent(key, native_value);
             },
             py::arg("key"),
             py::arg("value"))
        .def("replace",
             [](kor::Storage& storage, const std::string& key, py::handle value) {
                 const auto native_value = py_to_value(value);
                 py::gil_scoped_release release;
                 return storage.replace(key, native_value);
             },
             py::arg("key"),
             py::arg("value"))
        .def("compare_and_set",
             [](kor::Storage& storage, const std::string& key, py::handle expected, py::handle desired) {
                 const auto native_expected = py_to_value(expected);
                 const auto native_desired = py_to_value(desired);
                 py::gil_scoped_release release;
                 return storage.compare_and_set(key, native_expected, native_desired);
             },
             py::arg("key"),
             py::arg("expected"),
             py::arg("desired"))
        .def("get_and_set",
             [](kor::Storage& storage, const std::string& key, py::handle value) -> py::object {
                 const auto native_value = py_to_value(value);
                 std::optional<kor::Value> previous;
                 {
                     py::gil_scoped_release release;
                     previous = storage.get_and_set(key, native_value);
                 }
                 if (!previous.has_value()) {
                     return py::none();
                 }
                 return value_to_py(*previous);
             },
             py::arg("key"),
             py::arg("value"))
        .def("get_and_delete",
             [](kor::Storage& storage, const std::string& key) -> py::object {
                 std::optional<kor::Value> previous;
                 {
                     py::gil_scoped_release release;
                     previous = storage.get_and_delete(key);
                 }
                 if (!previous.has_value()) {
                     return py::none();
                 }
                 return value_to_py(*previous);
             },
             py::arg("key"))
        .def("transaction",
             [](kor::Storage& storage) {
                 py::gil_scoped_release release;
                 return storage.transaction();
             },
             py::keep_alive<0, 1>())
        .def("memory_stats", [](const kor::Storage& storage) {
            py::gil_scoped_release release;
            return storage.memory_stats();
        })
        .def("stats", [](const kor::Storage& storage) {
            py::gil_scoped_release release;
            return storage.stats();
        })
        .def("runtime_metrics",
             [](const kor::Storage& storage) {
                 kor::RuntimeMetricsSnapshot metrics;
                 {
                     py::gil_scoped_release release;
                     metrics = storage.runtime_metrics();
                 }
                 py::dict counters;
                 counters["successful_reads"] =
                     metrics.counters[static_cast<std::size_t>(kor::MetricCounter::kReads)];
                 counters["successful_writes"] =
                     metrics.counters[static_cast<std::size_t>(kor::MetricCounter::kWrites)];
                 counters["successful_deletes"] =
                     metrics.counters[static_cast<std::size_t>(kor::MetricCounter::kDeletes)];
                 counters["clear_operations"] =
                     metrics.counters[static_cast<std::size_t>(kor::MetricCounter::kClearOperations)];
                 counters["failed_operations"] =
                     metrics.counters[static_cast<std::size_t>(kor::MetricCounter::kFailedOperations)];
                 counters["transaction_commits"] =
                     metrics.counters[static_cast<std::size_t>(kor::MetricCounter::kTransactionCommits)];
                 counters["transaction_rollbacks"] =
                     metrics.counters[static_cast<std::size_t>(kor::MetricCounter::kTransactionRollbacks)];
                 counters["wal_records_written"] =
                     metrics.counters[static_cast<std::size_t>(kor::MetricCounter::kWalRecordsWritten)];
                 counters["wal_bytes_written"] =
                     metrics.counters[static_cast<std::size_t>(kor::MetricCounter::kWalBytesWritten)];
                 counters["checkpoint_count"] =
                     metrics.counters[static_cast<std::size_t>(kor::MetricCounter::kCheckpointCount)];
                 counters["checkpoint_failures"] =
                     metrics.counters[static_cast<std::size_t>(kor::MetricCounter::kCheckpointFailures)];
                 counters["recovery_count"] =
                     metrics.counters[static_cast<std::size_t>(kor::MetricCounter::kRecoveryCount)];
                 counters["index_build_count"] =
                     metrics.counters[static_cast<std::size_t>(kor::MetricCounter::kIndexBuildCount)];
                 counters["index_build_failures"] =
                     metrics.counters[static_cast<std::size_t>(kor::MetricCounter::kIndexBuildFailures)];
                 counters["indexed_queries"] =
                     metrics.counters[static_cast<std::size_t>(kor::MetricCounter::kIndexedQueries)];
                 counters["full_scan_queries"] =
                     metrics.counters[static_cast<std::size_t>(kor::MetricCounter::kFullScanQueries)];
                 counters["query_failures"] =
                     metrics.counters[static_cast<std::size_t>(kor::MetricCounter::kQueryFailures)];
                 counters["compaction_count"] =
                     metrics.counters[static_cast<std::size_t>(kor::MetricCounter::kCompactionCount)];
                 counters["compaction_failures"] =
                     metrics.counters[static_cast<std::size_t>(kor::MetricCounter::kCompactionFailures)];
                 counters["active_transactions"] =
                     metrics.counters[static_cast<std::size_t>(kor::MetricCounter::kActiveTransactions)];

                 constexpr std::array<const char*, static_cast<std::size_t>(kor::LatencyKind::kCount)>
                     latency_names = {"read", "write", "delete", "clear", "transaction_commit",
                                      "index_build", "query", "compaction"};
                 py::dict latencies;
                 for (std::size_t index = 0U; index < latency_names.size(); ++index) {
                     const auto& summary = metrics.latencies[index];
                     py::dict item;
                     item["count"] = summary.count;
                     item["minimum_ns"] = summary.minimum_ns;
                     item["maximum_ns"] = summary.maximum_ns;
                     item["mean_ns"] = summary.mean_ns;
                     py::list buckets;
                     for (std::size_t bucket = 0U; bucket < summary.buckets.size(); ++bucket) {
                         py::tuple entry(2);
                         entry[0] = kor::kLatencyBucketUpperBoundsNs[bucket];
                         entry[1] = summary.buckets[bucket];
                         buckets.append(std::move(entry));
                     }
                     item["buckets"] = std::move(buckets);
                     latencies[latency_names[index]] = std::move(item);
                 }

                 py::dict result;
                 result["counters"] = std::move(counters);
                 result["latencies"] = std::move(latencies);
                 result["current_committed_lsn"] = metrics.current_committed_lsn;
                 result["last_checkpoint_lsn"] = metrics.last_checkpoint_lsn;
                 result["active_index_count"] = metrics.active_index_count;
                 result["closed"] = metrics.closed;
                 result["recovery_required"] = metrics.recovery_required;
                 return result;
             })
        .def("storage_stats",
             [](const kor::Storage& storage) {
                 kor::StorageStats stats;
                 {
                     py::gil_scoped_release release;
                     stats = storage.stats();
                 }
                 py::dict result;
                 result["record_count"] = stats.record_count;
                 result["configured_limit_bytes"] = stats.configured_limit_bytes;
                 result["key_bytes"] = stats.key_bytes;
                 result["value_bytes"] = stats.value_bytes;
                 result["record_overhead_bytes"] = stats.record_overhead_bytes;
                 result["index_bytes_estimate"] = stats.index_bytes;
                 result["total_memory_bytes_estimate"] = stats.total_bytes;
                 result["file_size_bytes"] = stats.file_size_bytes;
                 result["logical_live_data_bytes_estimate"] = stats.logical_live_data_bytes_estimate;
                 result["wal_bytes_required"] = stats.wal_bytes_required;
                 result["snapshot_bytes_retained"] = stats.snapshot_bytes_retained;
                 result["allocated_region_bytes"] = stats.allocated_region_bytes;
                 result["reusable_free_space_bytes"] = stats.reusable_free_space_bytes;
                 result["obsolete_bytes_estimate"] = stats.obsolete_bytes_estimate;
                 result["internal_fragmentation_estimate"] = stats.internal_fragmentation_estimate;
                 result["last_checkpoint_lsn"] = stats.last_checkpoint_lsn;
                 result["compaction_generation"] = stats.compaction_generation;
                 result["last_auto_snapshot_error"] = stats.last_auto_snapshot_error;
                 result["physical_shrinking_supported"] = stats.physical_shrinking_supported;
                 result["closed"] = stats.closed;
                 return result;
             })
        .def("compact",
             [](kor::Storage& storage) {
                 kor::CompactionResult compacted;
                 {
                     py::gil_scoped_release release;
                     compacted = storage.compact();
                 }
                 py::dict result;
                 result["file_size_before_bytes"] = compacted.file_size_before_bytes;
                 result["file_size_after_bytes"] = compacted.file_size_after_bytes;
                 result["logical_wal_bytes_reclaimed"] = compacted.logical_wal_bytes_reclaimed;
                 result["file_length_reduction_bytes"] = compacted.file_length_reduction_bytes;
                 result["checkpoint_staging_memory_estimate_bytes"] =
                     compacted.checkpoint_staging_memory_estimate_bytes;
                 result["checkpoint_lsn"] = compacted.checkpoint_lsn;
                 result["compaction_generation"] = compacted.compaction_generation;
                 result["checkpoint_performed"] = compacted.checkpoint_performed;
                 result["file_physically_shrunk"] = compacted.file_physically_shrunk;
                 result["limitation"] = compacted.limitation;
                 return result;
             })
        .def("checkpoint",
             [](kor::Storage& storage) {
                 kor::CompactionResult checkpoint;
                 {
                     py::gil_scoped_release release;
                     checkpoint = storage.checkpoint();
                 }
                 py::dict result;
                 result["file_size_before_bytes"] = checkpoint.file_size_before_bytes;
                 result["file_size_after_bytes"] = checkpoint.file_size_after_bytes;
                 result["logical_wal_bytes_reclaimed"] = checkpoint.logical_wal_bytes_reclaimed;
                 result["file_length_reduction_bytes"] = checkpoint.file_length_reduction_bytes;
                 result["checkpoint_staging_memory_estimate_bytes"] =
                     checkpoint.checkpoint_staging_memory_estimate_bytes;
                 result["checkpoint_lsn"] = checkpoint.checkpoint_lsn;
                 result["compaction_generation"] = checkpoint.compaction_generation;
                 result["checkpoint_performed"] = checkpoint.checkpoint_performed;
                 result["file_physically_shrunk"] = checkpoint.file_physically_shrunk;
                 result["limitation"] = checkpoint.limitation;
                 return result;
             })
        .def("checkpoint_status",
             [](const kor::Storage& storage) {
                 const auto status = storage.checkpoint_status();
                 py::dict result;
                 result["last_attempt_lsn"] = status.last_attempt_lsn;
                 result["last_success_lsn"] = status.last_success_lsn;
                 result["last_attempt_duration_ns"] = status.last_attempt_duration_ns;
                 result["attempt_count"] = status.attempt_count;
                 result["success_count"] = status.success_count;
                 result["failure_count"] = status.failure_count;
                 result["in_progress"] = status.in_progress;
                 result["last_error"] = status.last_error;
                 return result;
             })
        .def("set_many",
             [](kor::Storage& storage, const py::dict& entries) {
                 std::vector<std::pair<std::string, kor::Value>> native_entries;
                 native_entries.reserve(entries.size());
                 for (const auto& entry : entries) {
                     native_entries.emplace_back(py::cast<std::string>(entry.first),
                                                  py_to_value(entry.second));
                 }
                 py::gil_scoped_release release;
                 storage.set_many_unique(native_entries);
             },
             py::arg("entries"))
        .def("delete_many",
             [](kor::Storage& storage, const std::vector<std::string>& keys) {
                const bool unique_keys =
                    keys.size() == std::unordered_set<std::string>(keys.begin(), keys.end()).size();
                py::gil_scoped_release release;
                if (unique_keys) {
                    storage.delete_many_unique(keys);
                } else {
                    storage.delete_many(keys);
                }
            },
            py::arg("keys"))
        .def("create_index",
             [](kor::Storage& storage,
                const std::string& name,
                const std::string& field,
                const std::string& index_type,
                bool unique) {
                 kor::SecondaryIndexType type;
                 if (index_type == "hash") {
                     type = kor::SecondaryIndexType::kHash;
                 } else if (index_type == "ordered") {
                     type = kor::SecondaryIndexType::kOrdered;
                 } else {
                     throw kor::InvalidIndexDefinitionError("Index type must be 'hash' or 'ordered'.");
                 }
                 py::gil_scoped_release release;
                 storage.create_index(name, field, type, unique);
             },
             py::arg("name"),
             py::arg("field"),
             py::arg("index_type") = "hash",
             py::arg("unique") = false)
        .def("create_composite_index",
             [](kor::Storage& storage,
                const std::string& name,
                const std::vector<std::string>& fields,
                bool unique) {
                 py::gil_scoped_release release;
                 storage.create_composite_index(name, fields, unique);
             },
             py::arg("name"),
             py::arg("fields"),
             py::arg("unique") = false)
        .def("drop_index",
             [](kor::Storage& storage, const std::string& name) {
                 py::gil_scoped_release release;
                 storage.drop_index(name);
             },
             py::arg("name"))
        .def("list_indexes", [](const kor::Storage& storage) {
            py::gil_scoped_release release;
            return storage.list_indexes();
        })
        .def("find",
             [](const kor::Storage& storage, const std::string& field, py::handle value) {
                 const auto native_value = py_to_value(value);
                 std::vector<std::pair<std::string, kor::Value>> results;
                 {
                     py::gil_scoped_release release;
                     results = storage.find(field, native_value);
                 }
                 py::list output;
                 for (const auto& [key, record] : results) {
                     output.append(py::make_tuple(key, value_to_py(record)));
                 }
                 return output;
             },
             py::arg("field"),
             py::arg("value"))
        .def("find_range",
             [](const kor::Storage& storage,
                const std::string& field,
                py::handle minimum,
                py::handle maximum,
                bool include_minimum,
                bool include_maximum) {
                 std::optional<kor::Value> native_minimum;
                 std::optional<kor::Value> native_maximum;
                 if (!minimum.is_none()) {
                     native_minimum = py_to_value(minimum);
                 }
                 if (!maximum.is_none()) {
                     native_maximum = py_to_value(maximum);
                 }
                 std::vector<std::pair<std::string, kor::Value>> results;
                 {
                     py::gil_scoped_release release;
                     results = storage.find_range(field, native_minimum, native_maximum,
                                                  include_minimum, include_maximum);
                 }
                 py::list output;
                 for (const auto& [key, record] : results) {
                     output.append(py::make_tuple(key, value_to_py(record)));
                 }
                 return output;
             },
             py::arg("field"),
             py::arg("minimum") = py::none(),
             py::arg("maximum") = py::none(),
             py::arg("include_minimum") = true,
             py::arg("include_maximum") = true)
        .def("find_composite",
             [](const kor::Storage& storage,
                const std::vector<std::string>& fields,
                const py::sequence& values) {
                 if (py::isinstance<py::str>(values) || py::isinstance<py::bytes>(values)) {
                     throw py::type_error("values must be a non-string sequence.");
                 }
                 if (values.size() != fields.size()) {
                     throw kor::InvalidIndexDefinitionError(
                         "Composite lookup fields and values must have the same length.");
                 }
                 std::vector<kor::Value> native_values;
                 native_values.reserve(values.size());
                 for (const auto value : values) {
                     native_values.push_back(py_to_value(value));
                 }
                 std::vector<std::pair<std::string, kor::Value>> results;
                 {
                     py::gil_scoped_release release;
                     results = storage.find_composite(fields, native_values);
                 }
                 py::list output;
                 for (const auto& [key, record] : results) {
                     output.append(py::make_tuple(key, value_to_py(record)));
                 }
                 return output;
             },
             py::arg("fields"),
             py::arg("values"))
        .def("find_composite_range",
             [](const kor::Storage& storage,
                const std::vector<std::string>& fields,
                py::handle minimum,
                py::handle maximum,
                bool include_minimum,
                bool include_maximum) {
                 const auto convert_bound = [](py::handle bound)
                     -> std::optional<std::vector<kor::Value>> {
                     if (bound.is_none()) {
                         return std::nullopt;
                     }
                     if (!py::isinstance<py::sequence>(bound) || py::isinstance<py::str>(bound) ||
                         py::isinstance<py::bytes>(bound)) {
                         throw py::type_error("Composite bounds must be sequences of values or None.");
                     }
                     const auto sequence = py::reinterpret_borrow<py::sequence>(bound);
                     std::vector<kor::Value> converted;
                     converted.reserve(sequence.size());
                     for (const auto value : sequence) {
                         converted.push_back(py_to_value(value));
                     }
                     return converted;
                 };
                 const auto native_minimum = convert_bound(minimum);
                 const auto native_maximum = convert_bound(maximum);
                 std::vector<std::pair<std::string, kor::Value>> results;
                 {
                     py::gil_scoped_release release;
                     results = storage.find_composite_range(fields,
                                                            native_minimum,
                                                            native_maximum,
                                                            include_minimum,
                                                            include_maximum);
                 }
                 py::list output;
                 for (const auto& [key, record] : results) {
                     output.append(py::make_tuple(key, value_to_py(record)));
                 }
                 return output;
             },
             py::arg("fields"),
             py::arg("minimum") = py::none(),
             py::arg("maximum") = py::none(),
             py::arg("include_minimum") = true,
             py::arg("include_maximum") = true)
        .def("scan",
             [](const kor::Storage& storage,
                const std::string& prefix,
                const std::optional<std::string>& after,
                std::size_t limit) {
                 kor::ScanPage page;
                 {
                     py::gil_scoped_release release;
                     page = storage.scan(prefix, after, limit);
                 }
                 py::list records;
                 for (const auto& [key, value] : page.records) {
                     records.append(py::make_tuple(key, value_to_py(value)));
                 }
                 py::dict result;
                 result["records"] = std::move(records);
                 result["next_cursor"] = page.next_cursor.has_value()
                                             ? py::cast(*page.next_cursor)
                                             : py::none();
                 result["has_more"] = page.has_more;
                 return result;
             },
             py::arg("prefix") = "",
             py::arg("after") = py::none(),
             py::arg("limit") = 100U)
        .def("__repr__", [](const kor::Storage&) { return "<kor._kor.Storage>"; });

    py::class_<kor::Config>(m, "Config")
        .def_readonly("path", &kor::Config::path)
        .def_readonly("name", &kor::Config::name)
        .def_readonly("max_memory_mib", &kor::Config::max_memory_mib)
        .def_readonly("snapshot_interval_seconds", &kor::Config::snapshot_interval_seconds);

    py::class_<kor::WalLog>(m, "WalLog")
        .def(py::init([](const std::filesystem::path& path, std::uint64_t max_memory_bytes) {
                 return new kor::WalLog(path, max_memory_bytes);
             }),
             py::arg("path") = std::filesystem::path(),
             py::arg("max_memory_bytes") = 64ULL * 1024ULL * 1024ULL)
        .def("open", &kor::WalLog::open)
        .def("close", &kor::WalLog::close)
        .def("is_open", &kor::WalLog::is_open)
        .def("next_lsn", &kor::WalLog::next_lsn)
        .def("append_set",
             [](kor::WalLog& wal, const std::string& key, py::handle value) {
                 return wal.append_set(key, py_to_value(value));
             },
             py::arg("key"),
             py::arg("value"))
        .def("append_delete", &kor::WalLog::append_delete, py::arg("key"))
        .def("append_clear", &kor::WalLog::append_clear)
        .def("replay", [](kor::WalLog& wal) { return wal.replay(); });

    py::class_<kor::Transaction>(m, "Transaction")
        .def(py::init([](kor::Storage& storage) {
                 py::gil_scoped_release release;
                 return new kor::Transaction(storage);
             }),
             py::arg("storage"),
             py::keep_alive<1, 2>())
        .def("active", [](const kor::Transaction& tx) {
            py::gil_scoped_release release;
            return tx.active();
        })
        .def("committed", [](const kor::Transaction& tx) {
            py::gil_scoped_release release;
            return tx.committed();
        })
        .def("rolled_back", [](const kor::Transaction& tx) {
            py::gil_scoped_release release;
            return tx.rolled_back();
        })
        .def("get",
             [](kor::Transaction& tx, const std::string& key) -> py::object {
                 std::optional<kor::Value> value;
                 {
                     py::gil_scoped_release release;
                     value = tx.get(key);
                 }
                 if (!value.has_value()) {
                     return py::none();
                 }
                 return value_to_py(*value);
             },
             py::arg("key"))
        .def("contains",
             [](const kor::Transaction& tx, const std::string& key) {
                 py::gil_scoped_release release;
                 return tx.contains(key);
             },
             py::arg("key"))
        .def("set",
             [](kor::Transaction& tx, const std::string& key, py::handle value) {
                 const auto native_value = py_to_value(value);
                 py::gil_scoped_release release;
                 tx.set(key, native_value);
             },
             py::arg("key"),
             py::arg("value"))
        .def("delete",
             [](kor::Transaction& tx, const std::string& key) {
                 py::gil_scoped_release release;
                 tx.erase(key);
             },
             py::arg("key"))
        .def("erase",
             [](kor::Transaction& tx, const std::string& key) {
                 py::gil_scoped_release release;
                 tx.erase(key);
             },
             py::arg("key"))
        .def("clear", [](kor::Transaction& tx) {
            py::gil_scoped_release release;
            tx.clear();
        })
        .def("commit", &commit_without_gil)
        .def("rollback", [](kor::Transaction& tx) {
            py::gil_scoped_release release;
            tx.rollback();
        })
        .def("__enter__", [](kor::Transaction& tx) -> kor::Transaction& { return tx; })
        .def("__exit__",
             [](kor::Transaction& tx, py::handle exc_type, py::handle exc_value, py::handle traceback) {
                 (void)exc_value;
                 (void)traceback;
                 if (exc_type.is_none()) {
                     if (tx.active()) {
                         commit_without_gil(tx);
                     }
                 } else if (tx.active()) {
                     tx.rollback();
                 }
                 return false;
             });

    py::class_<kor::Engine>(m, "Engine")
        .def(py::init<>())
        .def(py::init([](const std::filesystem::path& database_path,
                         std::uint64_t max_memory_bytes,
                         bool read_only) {
                 py::gil_scoped_release release;
                 return new kor::Engine(database_path, max_memory_bytes, read_only);
             }),
             py::arg("database_path") = std::filesystem::path(),
             py::arg("max_memory_bytes") = 64ULL * 1024ULL * 1024ULL,
             py::arg("read_only") = false)
        .def("start", &kor::Engine::start, "Start the engine lifecycle.")
        .def("close",
             [](kor::Engine& engine) {
                 py::gil_scoped_release release;
                 engine.close();
             },
             "Close the engine and release resources.")
        .def("is_started", &kor::Engine::is_started, "Return whether the engine is started.")
        .def("path", &kor::Engine::path)
        .def("storage",
             [](kor::Engine& engine) -> kor::Storage& { return engine.storage(); },
             py::return_value_policy::reference_internal)
        .def("__repr__", [](const kor::Engine&) { return "<kor._kor.Engine>"; });
}
