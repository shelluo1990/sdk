  * `StreamController` added setters for the `onListen`, `onPause`, `onResume`
    Also added `hasAbsolutePath`, `hasEmptyPath`, and `hasScheme` properties.
* Formatter (`dartfmt`)

  * Over 50 bugs fixed.

  * Optimized line splitter is much faster and produces better output on
    complex code.

* **BREAKING** The service protocol now sends JSON-RPC 2.0-compatible
  server-to-client events. To reflect this, the service protocol version is
  now 2.0.
