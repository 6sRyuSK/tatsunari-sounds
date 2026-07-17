// Factory UI · Visage — page harness.
//
// Served from source by dev_server.py (NOT baked into the wasm), so all of this
// can be edited and take effect on a plain page reload:
//   * window.ui.*  — thin wrappers over the C bridge (Module.ccall)
//   * theme.json hot reload  — poll with If-Modified-Since, apply via ui.reloadTheme
//   * live rebuild reload    — long-poll /events, reload the page when the wasm
//                              is rebuilt by `dev_server.py --watch`
(function () {
  function cc(name, ret, argTypes, args) { return Module.ccall(name, ret, argTypes, args); }

  // ---- bridge wrappers ------------------------------------------------------
  window.ui = {
    list:        function ()      { return JSON.parse(cc('ui_list_params', 'string', [], [])); },
    get:         function (id)    { return cc('ui_get_param', 'number', ['string'], [id]); },
    set:         function (id, v) { cc('ui_set_param', null, ['string', 'number'], [id, v]); },
    freeze:      function (b)     { cc('ui_freeze', null, ['number'], [b ? 1 : 0]); },
    reloadTheme: function (json)  { return cc('ui_reload_theme', 'number', ['string'], [json]) === 1; },
    lastError:   function ()      { return cc('ui_last_error', 'string', [], []); },
    accent:      function ()      { return cc('ui_get_accent', 'number', [], []) >>> 0; },
    widgetX:     function (id)    { return cc('ui_widget_x', 'number', ['string'], [id]); },
    widgetY:     function (id)    { return cc('ui_widget_y', 'number', ['string'], [id]); },

    // --- P2b ---
    // Rect (window px) of a control by param id or special name ("preset" /
    // "spectrum" / "valueSetting"); null if unknown.
    widgetRect:  function (key)   { var s = cc('ui_widget_rect', 'string', ['string'], [key]); try { return JSON.parse(s); } catch (e) { return null; } },
    setFont:     function (name)  { return cc('ui_set_font', 'number', ['string'], [name]) === 1; },
    font:        function ()      { return cc('ui_font', 'string', [], []); },
    feedSpectrum:function (phase) { cc('ui_feed_spectrum', null, ['number'], [phase]); },
    openDropdown:function (which) { return cc('ui_open_dropdown', 'number', ['number'], [which]) === 1; },
    dropdownOpen:function ()      { return cc('ui_dropdown_open', 'number', [], []) === 1; },
    dropdownCount:function ()     { return cc('ui_dropdown_item_count', 'number', [], []); },
    dropdownX:   function (i)     { return cc('ui_dropdown_x', 'number', ['number'], [i]); },
    dropdownRowY:function (i)     { return cc('ui_dropdown_row_y', 'number', ['number'], [i]); },
    presetIndex: function ()      { return cc('ui_preset_index', 'number', [], []); }
  };

  // ---- dev nicety 1: theme.json hot reload ----------------------------------
  var lastModified = null;
  function pollTheme() {
    var headers = lastModified ? { 'If-Modified-Since': lastModified } : {};
    fetch('theme.json', { headers: headers, cache: 'no-store' })
      .then(function (r) {
        if (r.status === 304) return null;
        lastModified = r.headers.get('Last-Modified') || lastModified;
        return r.text();
      })
      .then(function (text) {
        if (text && window.ui && window.Module && Module.ccall) {
          if (!window.ui.reloadTheme(text))
            console.warn('theme.json reload rejected:', window.ui.lastError());
        }
      })
      .catch(function () {})
      .then(function () { setTimeout(pollTheme, 300); });
  }

  // ---- dev nicety 2: live rebuild reload ------------------------------------
  function pollEvents(since) {
    fetch('events?since=' + since, { cache: 'no-store' })
      .then(function (r) { return r.json(); })
      .then(function (j) {
        if (j && typeof j.version === 'number' && j.version !== since) {
          console.log('[ui-dev] rebuild detected -> reloading');
          location.reload();
        } else {
          setTimeout(function () { pollEvents(since); }, 400);
        }
      })
      .catch(function () { setTimeout(function () { pollEvents(since); }, 1500); });
  }

  function start() {
    // __runtimeReady is set by the shell's onRuntimeInitialized. Calling any
    // export before it is set trips an -sASSERTIONS abort that halts the module.
    if (!(window.Module && Module.ccall && window.__runtimeReady)) { setTimeout(start, 100); return; }
    try {
      if (!window.ui.list().length) { setTimeout(start, 100); return; } // wait for main()
    } catch (e) { setTimeout(start, 100); return; }

    console.log('[ui-dev] bridge ready:', window.ui.list().map(function (p) { return p.id; }).join(', '));
    pollTheme();

    // Discover the current build version, then long-poll for changes. Servers
    // started without --watch simply never advance the version (harmless).
    fetch('events?since=-1', { cache: 'no-store' })
      .then(function (r) { return r.json(); })
      .then(function (j) { pollEvents(j && typeof j.version === 'number' ? j.version : 0); })
      .catch(function () { /* no --watch endpoint: skip live reload */ });
  }

  start();
})();
