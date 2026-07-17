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

  // ---- rs-editor bridge (only present in the rs-editor wasm; harmless if unused
  //      by the gallery, which never calls window.rs.*) --------------------------
  window.rs = {
    selectNode:  function (i)     { cc('rs_select_node', null, ['number'], [i]); },
    selectedNode:function ()      { return cc('rs_selected_node', 'number', [], []); },
    nodeX:       function (i)     { return cc('rs_node_x', 'number', ['number'], [i]); },
    nodeY:       function (i)     { return cc('rs_node_y', 'number', ['number'], [i]); },
    listenNode:  function ()      { return cc('rs_listen_node', 'number', [], []); },
    displaySmooth:function ()     { return cc('rs_display_smooth', 'number', [], []); },
    abSlot:      function ()      { return cc('rs_ab_slot', 'number', [], []); },
    setAb:       function (s)     { cc('rs_set_ab', null, ['number'], [s]); },
    copyAb:      function ()      { cc('rs_copy_ab', null, [], []); },
    presetIndex: function ()      { return cc('rs_preset_index', 'number', [], []); },
    presetLoad:  function (i)     { cc('rs_preset_load', null, ['number'], [i]); },
    uiEdit:      function (id, v) { cc('rs_ui_edit', null, ['string', 'number'], [id, v]); },
    undo:        function ()      { cc('rs_undo', null, [], []); },
    redo:        function ()      { cc('rs_redo', null, [], []); },
    canUndo:     function ()      { return cc('rs_can_undo', 'number', [], []) === 1; },
    canRedo:     function ()      { return cc('rs_can_redo', 'number', [], []) === 1; },
    setClock:    function (s)     { cc('rs_set_clock', null, ['number'], [s]); },
    pump:        function ()      { cc('rs_pump', null, [], []); },
    openDropdown:function (w)     { return cc('rs_open_dropdown', 'number', ['number'], [w]) === 1; },
    dropdownOpen:function ()      { return cc('rs_dropdown_open', 'number', [], []) === 1; },
    dropdownCount:function ()     { return cc('rs_dropdown_count', 'number', [], []); },
    dropdownX:   function (i)     { return cc('rs_dropdown_x', 'number', ['number'], [i]); },
    dropdownRowY:function (i)     { return cc('rs_dropdown_row_y', 'number', ['number'], [i]); },
    plotRect:    function ()      { var s = cc('rs_plot_rect', 'string', [], []); try { return JSON.parse(s); } catch (e) { return null; } },
    setSize:     function (w, h)  { cc('rs_set_size', null, ['number', 'number'], [w, h]); }
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
