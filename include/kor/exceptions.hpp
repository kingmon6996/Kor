#pragma once

#include <stdexcept>

namespace kor {

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class InitializationError : public Error {
public:
    using Error::Error;
};

class InvalidConfigurationError : public Error {
public:
    using Error::Error;
};

class InvalidKeyError : public Error {
public:
    using Error::Error;
};

class InvalidValueError : public Error {
public:
    using Error::Error;
};

class SerializationError : public Error {
public:
    using Error::Error;
};

class MemoryLimitExceededError : public Error {
public:
    using Error::Error;
};

class StorageClosedError : public Error {
public:
    using Error::Error;
};

class ReadOnlyError : public Error {
public:
    using Error::Error;
};

class DatabaseFormatError : public Error {
public:
    using Error::Error;
};

class UnsupportedFormatVersionError : public DatabaseFormatError {
public:
    using DatabaseFormatError::DatabaseFormatError;
};

class PersistenceError : public Error {
public:
    using Error::Error;
};

class RecoveryError : public Error {
public:
    using Error::Error;
};

class TransactionError : public Error {
public:
    using Error::Error;
};

class TransactionStateError : public TransactionError {
public:
    using TransactionError::TransactionError;
};

class TransactionTooLargeError : public TransactionError {
public:
    using TransactionError::TransactionError;
};

class TransactionCommitError : public TransactionError {
public:
    using TransactionError::TransactionError;
};

class IndexError : public Error {
public:
    using Error::Error;
};

class InvalidIndexDefinitionError : public IndexError {
public:
    using IndexError::IndexError;
};

class IndexExistsError : public IndexError {
public:
    using IndexError::IndexError;
};

class IndexNotFoundError : public IndexError {
public:
    using IndexError::IndexError;
};

class UniqueConstraintError : public IndexError {
public:
    using IndexError::IndexError;
};

class UnsupportedIndexValueError : public IndexError {
public:
    using IndexError::IndexError;
};

}  // namespace kor
