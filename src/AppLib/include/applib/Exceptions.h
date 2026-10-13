#pragma once

#include <string>
#include <stdexcept>

#include "Platform.h"

namespace applib {

class Exception : public std::runtime_error {
public:
  explicit Exception(std::string const& message)
      : std::runtime_error(message) {
  }
};

class NotImplementedException : public Exception {
public:
  NotImplementedException()
      : Exception("Not implemented yet.") {
  }

  explicit NotImplementedException(std::string const& function)
      : Exception(function + " is not implemented yet.") {
  }

  NotImplementedException(std::string const& function, std::string const& msg)
      : Exception(function + ": " + msg + " is not implemented yet.") {
  }
};

}  // namespace applib
