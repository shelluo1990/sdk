// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:async';
import 'package:expect/expect.dart';
import 'package:async_helper/async_helper.dart';
import 'compiler_helper.dart';

const String TEST_ONE = r"""
main() {
  var f = use;
  if (false) {
    // This statement and the use of 'foo' should be optimized away, causing
    // 'foo' to be absent from the final code.
    f(foo);
  }
  f(bar);
}

foo() => 'Tarantula!';
bar() => 'Coelacanth!';

use(x) {
  print(x());
}
""";


main() {
  asyncTest(() => Future.wait([
      compileAll(TEST_ONE).then((String generated) {
        Expect.isFalse(generated.contains('Tarantula!'),
            "failed to remove 'foo'");
        Expect.isTrue(generated.contains('Coelacanth!'));
      }),
  ]));
}
