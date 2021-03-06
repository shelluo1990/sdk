// Copyright (c) 2016, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file

part of models;

abstract class Breakpoint extends Object {
  /// A number identifying this breakpoint to the user.
  int get number;

  /// Has this breakpoint been assigned to a specific program location?
  bool get resolved;
}
