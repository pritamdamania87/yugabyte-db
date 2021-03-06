//--------------------------------------------------------------------------------------------------
// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
//
// Wrapper for BISON location.hh. This module implements the abstract interface YBLocation, which
// identifies the location of tokens in SQL statement. When generating parse tree, the parser
// (BISON) will save the location values in the tree nodes.
//--------------------------------------------------------------------------------------------------

#ifndef YB_QL_PARSER_LOCATION_H_
#define YB_QL_PARSER_LOCATION_H_

#include "yb/ql/ptree/yb_location.h"
#include "yb/ql/parser/location.hh"

namespace yb {
namespace ql {

class Location : public YBLocation {
 public:
  //------------------------------------------------------------------------------------------------
  // Public types.
  typedef MCSharedPtr<Location> SharedPtr;
  typedef MCSharedPtr<const Location> SharedPtrConst;

  //------------------------------------------------------------------------------------------------
  // Constructor.
  explicit Location(const location& loc) : loc_(loc) {
  }
  Location(MemoryContext *memctx, const location& loc) : loc_(loc) {
  }
  virtual ~Location() {
  }

  // shared_ptr support.
  template<typename... TypeArgs>
  inline static Location::SharedPtr MakeShared(MemoryContext *memctx, TypeArgs&&... args) {
    return MCMakeShared<Location>(memctx, std::forward<TypeArgs>(args)...);
  }

  // Abstract interface.
  virtual int BeginLine() const {
    return loc_.begin.line;
  }

  virtual int BeginColumn() const {
    return loc_.begin.column;
  }

  virtual int EndLine() const {
    return loc_.end.line;
  }

  virtual int EndColumn() const {
    return loc_.end.column;
  }

 private:
  location loc_;
};

} // namespace ql
} // namespace yb

#endif // YB_QL_PARSER_LOCATION_H_
