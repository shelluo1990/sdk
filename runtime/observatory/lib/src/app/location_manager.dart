// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

part of app;

class LocationManager extends Observable {
  final _defaultPath = '/vm';

  final ObservatoryApplication _app;

  /// [internalArguments] are parameters specified after a '---' in the
  /// application URL.
  final Map<String, String> internalArguments = new Map<String, String>();

  Uri _uri;
  /// [uri] is the application uri. Application uris consist of a path and
  /// the queryParameters map.
  Uri get uri => _uri;

  LocationManager(this._app) {
    window.onPopState.listen(_onBrowserNavigation);
    // Determine initial application path.
    var applicationPath = '${window.location.hash}';
    if ((window.location.hash == '') || (window.location.hash == '#')) {
      // Observatory has loaded but no application path has been specified,
      // use the default.
      applicationPath = makeLink(_defaultPath);
    }
    // Update current application path.
    window.history.replaceState(applicationPath,
                                document.title,
                                applicationPath);
    _updateApplicationLocation(applicationPath);
  }

  bool getBoolParameter(String name, bool defaultValue) {
    var value = uri.queryParameters[name];
    if ("true" == value) return true;
    if ("false" == value) return false;
    return defaultValue;
  }

  /// Called whenever the browser changes the location bar (e.g. forward or
  /// back button press).
  void _onBrowserNavigation(PopStateEvent event) {
    _updateApplicationLocation(window.location.hash);
    _visit();
  }

  /// Given an application url, generate an href link.
  String makeLink(String url) => '#$url';

  /// Update the application location. After this function returns,
  /// [uri] and [debugArguments] will be updated.
  _updateApplicationLocation(String url) {
    if (url == makeLink('/vm-connect')) {
      // When we go to the vm-connect page, drop all notifications.
      _app.notifications.deleteAll();
    }

    // Chop off leading '#'.
    if (url.startsWith('#')) {
      url = url.substring(1);
    }
    // Fall through handles '#/'
    // Chop off leading '/'.
    if (url.startsWith('/')) {
      url = url.substring(1);
    }
    // Parse out debug arguments.
    if (url.contains('---')) {
      var chunks = url.split('---');
      url = chunks[0];
      if ((chunks.length > 1) && (chunks[1] != '')) {
        internalArguments.clear();
        try {
          internalArguments.addAll(Uri.splitQueryString(chunks[1]));
        } catch (e) {
          Logger.root.warning('Could not parse debug arguments ${e}');
        }
      }
    }
    _uri = Uri.parse(url);
  }

  /// Add [url] to the browser history.
  _addToBrowserHistory(String url) {
    window.history.pushState(url, document.title, url);
  }

  /// Notify the current page that something has changed.
  _visit() {
    runZoned(() => _app._visit(_uri, internalArguments),
             onError: (e, st) {
      if (e is IsolateNotFound) {
        var newPath = ((_app.vm == null || _app.vm.isDisconnected)
                       ? '/vm-connect' : '/isolate-reconnect');
        var parameters = {};
        parameters.addAll(_uri.queryParameters);
        parameters['originalPath'] = _uri.path;
        parameters['originalIsolateId'] = parameters['isolateId'];
        var generatedUri = new Uri(path: newPath, queryParameters: parameters);
        go(makeLink(generatedUri.toString()), true);
        return;
      }
      // Surface any uncaught exceptions.
      _app.handleException(e, st);
    });
  }

  /// Navigate to [url].
  void go(String url, [bool addToBrowserHistory = true]) {
    if ((url != makeLink('/vm-connect')) &&
        (_app.vm == null || _app.vm.isDisconnected)) {
      if (!window.confirm('Connection with VM has been lost. '
                          'Proceeding will lose current page.')) {
        return;
      }
      url = makeLink('/vm-connect');
    }

    if (addToBrowserHistory) {
      _addToBrowserHistory(url);
    }
    _updateApplicationLocation(url);
    _visit();
  }

  /// Starting with the current uri path and queryParameters, update
  /// queryParameters present in [updateParameters], then generate a new uri
  /// and navigate to that.
  goReplacingParameters(Map updatedParameters, [bool addToBrowserHistory = true]) {
    go(makeLinkReplacingParameters(updatedParameters), addToBrowserHistory);
  }

  makeLinkReplacingParameters(Map updatedParameters) {
    var parameters = new Map.from(_uri.queryParameters);
    updatedParameters.forEach((k, v) {
      parameters[k] = v;
    });
    // Ensure path starts with a slash.
    var path = uri.path.startsWith('/') ? uri.path : '/${uri.path}';
    var generatedUri = new Uri(path: path, queryParameters: parameters);
    return makeLink(generatedUri.toString());
  }

  goForwardingParameters(String newPath, [bool addToBrowserHistory = true]) {
    go(makeLinkForwardingParameters(newPath), addToBrowserHistory);
  }

  makeLinkForwardingParameters(String newPath) {
    var parameters = _uri.queryParameters;
    var generatedUri = new Uri(path: newPath, queryParameters: parameters);
    return makeLink(generatedUri.toString());
  }

  /// Utility event handler when clicking on application url link.
  void onGoto(MouseEvent event) {
    if ((event.button > 0) ||
        event.metaKey      ||
        event.ctrlKey      ||
        event.shiftKey     ||
        event.altKey) {
      // Mouse event is not a left-click OR
      // mouse event is a left-click with a modifier key:
      // let browser handle.
      return;
    }
    event.preventDefault();
    // 'currentTarget' is the dom element that would process the event.
    // If we use 'target' we might get an <em> element or somesuch.
    var target = event.currentTarget;
    go(target.attributes['href']);
  }
}
